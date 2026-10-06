#include "minidb/transaction_manager.hpp"
#include "minidb/checkpoint_manager.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <vector>

namespace minidb {
TransactionManager::SessionGuard::SessionGuard(std::shared_ptr<SessionContext> context)
    : context_(std::move(context)), lock_(context_->mutex) {}
TransactionManager::TransactionManager(RecoveryCoordinator* recovery, CheckpointManager* checkpoints)
    : recovery_(recovery), checkpoints_(checkpoints) {
    if (checkpoints_) checkpoints_->attachAccessGate(gate_);
}
TransactionManager::~TransactionManager() {
    try { shutdown(); } catch (...) {}
}
std::shared_ptr<TransactionManager::SessionContext> TransactionManager::sessionContext(SessionId session) {
    std::lock_guard lock(sessionsMutex_);
    if (stopped_ && !sessions_.contains(session)) throw AccessCancelled();
    auto& context = sessions_[session];
    if (!context) context = std::make_shared<SessionContext>();
    return context;
}
std::shared_ptr<TransactionManager::SessionContext> TransactionManager::findSession(SessionId session) const {
    std::lock_guard lock(sessionsMutex_);
    const auto found = sessions_.find(session);
    return found == sessions_.end() ? nullptr : found->second;
}
TransactionManager::SessionGuard TransactionManager::lockSession(SessionId session) {
    SessionGuard guard(sessionContext(session));
    if (guard.context_->closed) throw AccessCancelled();
    return guard;
}
DatabaseAccessGate::CancelProbe TransactionManager::cancellation(SessionContext& session) {
    return [&session] { return session.cancelRequested.load()
        || (session.cancelled && session.cancelled()); };
}
void TransactionManager::setCancellationProbe(SessionId session, DatabaseAccessGate::CancelProbe probe) {
    auto guard = lockSession(session);
    guard.context_->cancelled = std::move(probe);
}
void TransactionManager::requireSession(SessionId session) const {
    if (failed_) throw std::runtime_error("Transaction completion failed; database must be reopened");
    if (stopped_) throw AccessCancelled();
    if (const auto context = findSession(session)) {
        std::lock_guard lock(context->mutex);
        if (context->closed) throw AccessCancelled();
    }
}
void TransactionManager::requireWriter(const SessionContext& session) const {
    assert(session.lease && session.lease->owns(gate_) && session.lease->mode() == AccessMode::ReadWrite);
    if (!session.lease || !session.lease->owns(gate_) || session.lease->mode() != AccessMode::ReadWrite) {
        throw std::logic_error("Recovery mutation requires the exclusive database lease");
    }
    if (recovery_) {
        const auto entries = recovery_->transactionSnapshot();
        if (std::none_of(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.transactionId == session.writerId;
            })) {
            throw std::logic_error("Recovery transaction is missing for writer session");
        }
    }
}
void TransactionManager::begin(SessionId session, AccessMode mode) {
    auto guard = lockSession(session);
    auto& context = *guard.context_;
    requireSession(session);
    if (context.explicitMode) throw std::logic_error("Nested BEGIN is not supported");
    if (mode == AccessMode::ReadWrite && !recovery_) {
        throw std::logic_error("READ WRITE transactions require WAL-enabled SqlEngine");
    }
    auto lease = mode == AccessMode::ReadOnly ? gate_.acquireShared(cancellation(context))
                                             : gate_.acquireExclusive(cancellation(context));
    requireSession(session);
    if (mode == AccessMode::ReadWrite) {
        assert(!recovery_->hasActiveStatement());
        if (recovery_->hasActiveStatement()) throw std::logic_error("SQL admits only one physical writer");
        context.writerId = recovery_->beginTransaction();
        recovery_->bindTransaction(context.writerId);
        context.initialWalBytes = recovery_->stats().walTotalBytesGenerated;
    }
    context.lease.emplace(std::move(lease));
    context.explicitMode = mode;
    ++activeCount_;
    { std::lock_guard lock(statsMutex_); ++stats_.explicitTransactionsBegun; }
    if (mode == AccessMode::ReadWrite) captureWriterStats(context.writerId);
}
void TransactionManager::captureWriterStats(TransactionId id) {
    if (!recovery_) return;
    std::lock_guard lock(statsMutex_);
    const auto bytes = recovery_->originalBeforeImageBytes(id);
    stats_.originalBeforeImageBytes = std::max(stats_.originalBeforeImageBytes, bytes);
    stats_.originalBeforeImagePages = std::max(stats_.originalBeforeImagePages, bytes / database_format::PAGE_SIZE);
    stats_.peakTransactionRecoveryBytes = std::max(stats_.peakTransactionRecoveryBytes,
                                                  recovery_->peakTransactionRecoveryBytes(id));
    writer_ = {id, recovery_->hasMaterializedWalBegin(id),
               recovery_->lastLsn(id), recovery_->touchedPageCount(id), recovery_->transactionWalBytes(id)};
}
void TransactionManager::safeBoundary(SessionContext& session) noexcept {
    if (checkpoints_) static_cast<void>(checkpoints_->onTransactionCompleted(&*session.lease));
}
void TransactionManager::endExplicit(SessionContext& context) {
    const bool wasWriter = context.explicitMode == AccessMode::ReadWrite;
    context.explicitMode.reset();
    context.writerId = INVALID_TRANSACTION_ID;
    --activeCount_;
    if (wasWriter) {
        { std::lock_guard lock(statsMutex_); writer_ = {}; }
        safeBoundary(context);
    }
    context.lease.reset();
}
void TransactionManager::commit(SessionId session) {
    auto guard = lockSession(session);
    auto& context = *guard.context_;
    requireSession(session);
    if (!context.explicitMode) throw std::logic_error("No explicit transaction is active");
    bool readOnly = context.explicitMode == AccessMode::ReadOnly;
    if (!readOnly) {
        requireWriter(context);
        recovery_->prepareTransaction(context.writerId); captureWriterStats(context.writerId);
        readOnly = !recovery_->hasMaterializedWalBegin(context.writerId);
        try { recovery_->commitTransaction(context.writerId); }
        catch (...) { failed_ = true; gate_.shutdown(); throw; }
        std::lock_guard lock(statsMutex_);
        stats_.explicitTransactionWalBytes += recovery_->stats().walTotalBytesGenerated - context.initialWalBytes;
    }
    { std::lock_guard lock(statsMutex_);
      ++stats_.explicitTransactionsCommitted;
      if (readOnly) ++stats_.readOnlyExplicitTransactions; }
    endExplicit(context);
}
void TransactionManager::finishRollback(SessionContext& context) {
    bool readOnly = context.explicitMode == AccessMode::ReadOnly;
    if (!readOnly) {
        requireWriter(context); captureWriterStats(context.writerId);
        try { recovery_->rollbackTransaction(context.writerId); }
        catch (...) { failed_ = true; gate_.shutdown(); throw; }
        const auto bytes = recovery_->stats().walTotalBytesGenerated - context.initialWalBytes;
        readOnly = bytes == 0;
        std::lock_guard lock(statsMutex_); stats_.explicitTransactionWalBytes += bytes;
    }
    { std::lock_guard lock(statsMutex_);
      ++stats_.explicitTransactionsRolledBack;
      if (readOnly) ++stats_.readOnlyExplicitTransactions; }
    endExplicit(context);
}
void TransactionManager::rollback(SessionId session) {
    auto guard = lockSession(session); requireSession(session);
    if (!guard.context_->explicitMode) throw std::logic_error("No explicit transaction is active");
    finishRollback(*guard.context_);
}
CheckpointId TransactionManager::checkpoint(SessionId session, CheckpointMode mode) {
    auto guard = lockSession(session);
    requireSession(session);
    auto& context = *guard.context_;
    if (!checkpoints_) throw std::logic_error("Checkpoint manager is unavailable");
    if (!context.explicitMode) return checkpoints_->checkpoint(mode);
    if (context.explicitMode != AccessMode::ReadWrite) {
        throw std::logic_error("Read-only sessions cannot publish checkpoints");
    }
    requireWriter(context);
    return checkpoints_->checkpoint(mode, *context.lease);
}
void TransactionManager::beginMutation(SessionId session) {
    auto guard = lockSession(session); requireSession(session);
    auto& context = *guard.context_;
    if (context.explicitMode == AccessMode::ReadOnly) {
        throw std::logic_error("Mutation is not permitted in a READ ONLY transaction; no lock upgrade is supported");
    }
    if (!context.explicitMode) {
        context.lease.emplace(gate_.acquireExclusive(cancellation(context)));
        requireSession(session);
        if (recovery_) {
            assert(!recovery_->hasActiveStatement());
            if (recovery_->hasActiveStatement()) throw std::logic_error("SQL admits only one physical writer");
            context.writerId = recovery_->beginTransaction();
            recovery_->bindTransaction(context.writerId);
        }
    }
    requireWriter(context);
    if (recovery_) recovery_->beginMutation(context.writerId);
}
void TransactionManager::completeMutation(SessionId session) {
    auto guard = lockSession(session); requireSession(session);
    auto& context = *guard.context_; requireWriter(context);
    if (recovery_) {
        if (context.explicitMode) {
            recovery_->prepareTransaction(context.writerId); captureWriterStats(context.writerId);
            std::lock_guard lock(statsMutex_); ++stats_.explicitTransactionStatements;
        } else {
            try { recovery_->commitTransaction(context.writerId); }
            catch (...) { failed_ = true; gate_.shutdown(); throw; }
            std::lock_guard lock(statsMutex_); ++stats_.implicitTransactionsCommitted;
        }
        if (checkpoints_) static_cast<void>(checkpoints_->onStatementCommitted(&*context.lease));
    }
    if (!context.explicitMode) { context.writerId = INVALID_TRANSACTION_ID; context.lease.reset(); }
}
void TransactionManager::failMutation(SessionId session) {
    auto guard = lockSession(session); requireSession(session);
    auto& context = *guard.context_;
    if (!context.lease) return;
    if (context.explicitMode) {
        finishRollback(context);
        std::lock_guard lock(statsMutex_); ++stats_.explicitTransactionsAutoRolledBackOnError;
    } else {
        if (recovery_ && recovery_->hasActiveStatement()) {
            requireWriter(context);
            try { recovery_->rollbackTransaction(context.writerId); }
            catch (...) { failed_ = true; gate_.shutdown(); throw; }
            safeBoundary(context);
        }
        context.writerId = INVALID_TRANSACTION_ID; context.lease.reset();
    }
}
void TransactionManager::beginRead(SessionId session) {
    auto guard = lockSession(session); requireSession(session);
    if (!guard.context_->explicitMode) guard.context_->lease.emplace(gate_.acquireShared(cancellation(*guard.context_)));
    requireSession(session);
}
void TransactionManager::completeRead(SessionId session) {
    auto guard = lockSession(session);
    if (guard.context_->explicitMode) {
        std::lock_guard lock(statsMutex_); ++stats_.explicitTransactionStatements;
    } else guard.context_->lease.reset();
}
void TransactionManager::failRead(SessionId session) {
    auto guard = lockSession(session);
    if (!guard.context_->explicitMode) guard.context_->lease.reset();
}
void TransactionManager::closeSession(SessionId session, SessionCloseReason reason) {
    auto context = findSession(session);
    if (!context) return;
    context->cancelRequested = true; // Cancel admission before waiting for its session latch.
    std::lock_guard lock(context->mutex);
    if (context->explicitMode) {
        if (failed_) throw std::runtime_error("Transaction cleanup failed; database requires reopen");
        const bool writer = context->explicitMode == AccessMode::ReadWrite;
        finishRollback(*context);
        if (writer) {
            std::lock_guard statsLock(statsMutex_);
            if (reason == SessionCloseReason::Shutdown) ++stats_.shutdownRollbacks;
            else ++stats_.disconnectRollbacks;
        }
    } else if (context->lease && context->lease->mode() == AccessMode::ReadWrite
               && recovery_ && recovery_->hasActiveStatement()) {
        // An externally requested shutdown can interrupt implicit completion.
        // Do not release exclusion while that physical transaction is unfinished.
        if (failed_) throw std::runtime_error("Transaction cleanup failed; database requires reopen");
        requireWriter(*context);
        try { recovery_->rollbackTransaction(context->writerId); }
        catch (...) { failed_ = true; gate_.shutdown(); throw; }
        safeBoundary(*context);
    }
    context->closed = true;
    context->lease.reset();
    std::lock_guard sessionsLock(sessionsMutex_);
    if (const auto found = sessions_.find(session); found != sessions_.end() && found->second == context) {
        sessions_.erase(found);
    }
}
void TransactionManager::shutdown() {
    stopped_ = true; gate_.shutdown();
    std::vector<SessionId> sessions;
    { std::lock_guard lock(sessionsMutex_); for (const auto& entry : sessions_) sessions.push_back(entry.first); }
    for (auto session : sessions) {
        closeSession(session, SessionCloseReason::Shutdown);
    }
}
bool TransactionManager::hasActiveExplicitTransaction(SessionId session) const {
    return accessMode(session).has_value();
}
std::optional<AccessMode> TransactionManager::accessMode(SessionId session) const {
    if (const auto context = findSession(session)) { std::lock_guard lock(context->mutex); return context->explicitMode; }
    return std::nullopt;
}
TransactionId TransactionManager::activeTransactionId() const { std::lock_guard lock(statsMutex_); return writer_.id; }
bool TransactionManager::hasMaterializedWalBegin() const { std::lock_guard lock(statsMutex_); return writer_.logged; }
Lsn TransactionManager::lastLsn() const { std::lock_guard lock(statsMutex_); return writer_.last; }
std::size_t TransactionManager::touchedPageCount() const { std::lock_guard lock(statsMutex_); return writer_.touched; }
std::uint64_t TransactionManager::transactionWalBytes() const { std::lock_guard lock(statsMutex_); return writer_.bytes; }
ExplicitTransactionStats TransactionManager::stats() const { std::lock_guard lock(statsMutex_); return stats_; }
void TransactionManager::resetStats() { std::lock_guard lock(statsMutex_); stats_ = {}; }
} // namespace minidb
