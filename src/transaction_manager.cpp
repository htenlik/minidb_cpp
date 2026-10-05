#include "minidb/transaction_manager.hpp"

#include "minidb/checkpoint_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace minidb {

void TransactionManager::requireSession(SessionId session) const {
    if (failed_) throw std::runtime_error("Transaction completion failed; database must be reopened");
    if (active_ && active_->owner != session) {
        throw std::logic_error("Another session owns the active transaction");
    }
    if (recovery_ != nullptr && recovery_->rollbackActive()) {
        throw std::runtime_error("Rollback did not complete; database must be reopened");
    }
}

void TransactionManager::requireActive(SessionId session) const {
    requireSession(session);
    if (!active_) throw std::logic_error("No explicit transaction is active");
}

void TransactionManager::begin(SessionId session) {
    requireSession(session);
    if (active_) throw std::logic_error("Nested BEGIN is not supported");
    if (recovery_ == nullptr) {
        throw std::logic_error("Explicit transactions require WAL-enabled SqlEngine");
    }
    recovery_->beginStatement();
    active_ = TransactionContext{session, recovery_->stats().walTotalBytesGenerated};
    ++stats_.explicitTransactionsBegun;
}

void TransactionManager::captureMemoryStats() {
    if (recovery_ == nullptr) return;
    const auto bytes = recovery_->originalBeforeImageBytes();
    stats_.originalBeforeImageBytes = std::max(stats_.originalBeforeImageBytes, bytes);
    stats_.originalBeforeImagePages = std::max(
        stats_.originalBeforeImagePages, bytes / database_format::PAGE_SIZE);
    stats_.peakTransactionRecoveryBytes = std::max(
        stats_.peakTransactionRecoveryBytes, recovery_->peakTransactionRecoveryBytes());
}

void TransactionManager::safeBoundary() noexcept {
    if (checkpoints_ != nullptr) static_cast<void>(checkpoints_->onTransactionCompleted());
}

void TransactionManager::commit(SessionId session) {
    requireActive(session);
    recovery_->prepareStatement();
    captureMemoryStats();
    const bool readOnly = !recovery_->hasMaterializedWalBegin();
    try {
        recovery_->commitStatement();
    } catch (...) {
        // An appended COMMIT may have become durable even if I/O reported an
        // error. Do not append ABORT or accept more statements with that chain.
        failed_ = true;
        throw;
    }
    stats_.explicitTransactionWalBytes += recovery_->stats().walTotalBytesGenerated
        - active_->initialWalBytes;
    active_.reset();
    ++stats_.explicitTransactionsCommitted;
    if (readOnly) ++stats_.readOnlyExplicitTransactions;
    safeBoundary();
}

void TransactionManager::finishRollback() {
    captureMemoryStats();
    try {
        recovery_->rollbackStatement();
    } catch (...) {
        failed_ = true;
        throw;
    }
    // A failing first mutation can materialize BEGIN during rollback preparation.
    const bool readOnly = recovery_->stats().walTotalBytesGenerated == active_->initialWalBytes;
    stats_.explicitTransactionWalBytes += recovery_->stats().walTotalBytesGenerated
        - active_->initialWalBytes;
    active_.reset();
    ++stats_.explicitTransactionsRolledBack;
    if (readOnly) ++stats_.readOnlyExplicitTransactions;
    safeBoundary();
}

void TransactionManager::rollback(SessionId session) {
    requireActive(session);
    finishRollback();
}

void TransactionManager::beginMutation(SessionId session) {
    requireSession(session);
    if (!active_ && recovery_ != nullptr) recovery_->beginStatement();
}

void TransactionManager::completeMutation(SessionId session) {
    requireSession(session);
    if (recovery_ == nullptr) return;
    if (active_) {
        recovery_->prepareStatement();
        captureMemoryStats();
        ++stats_.explicitTransactionStatements;
    } else {
        try {
            recovery_->commitStatement();
        } catch (...) {
            failed_ = true;
            throw;
        }
        ++stats_.implicitTransactionsCommitted;
    }
    if (checkpoints_ != nullptr) static_cast<void>(checkpoints_->onStatementCommitted());
}

void TransactionManager::failMutation(SessionId session) {
    requireSession(session);
    if (recovery_ == nullptr || !recovery_->hasActiveStatement()) return;
    if (active_) {
        finishRollback();
        ++stats_.explicitTransactionsAutoRolledBackOnError;
    } else {
        try {
            recovery_->rollbackStatement();
        } catch (...) {
            failed_ = true;
            throw;
        }
        safeBoundary();
    }
}

void TransactionManager::completeRead(SessionId session) {
    requireSession(session);
    if (active_) ++stats_.explicitTransactionStatements;
}

void TransactionManager::closeSession(SessionId session) {
    if (!active_ || active_->owner != session) return;
    requireSession(session);
    finishRollback();
    ++stats_.disconnectRollbacks;
}

void TransactionManager::shutdown() {
    if (!active_) return;
    requireSession(active_->owner);
    finishRollback();
    ++stats_.shutdownRollbacks;
}

TransactionId TransactionManager::activeTransactionId() const noexcept {
    return active_ ? recovery_->activeTransactionId() : INVALID_TRANSACTION_ID;
}
bool TransactionManager::hasMaterializedWalBegin() const noexcept {
    return active_ && recovery_->hasMaterializedWalBegin();
}
Lsn TransactionManager::lastLsn() const noexcept {
    return active_ ? recovery_->lastLsn() : INVALID_LSN;
}
std::size_t TransactionManager::touchedPageCount() const noexcept {
    return active_ ? recovery_->touchedPageCount() : 0;
}
std::uint64_t TransactionManager::transactionWalBytes() const noexcept {
    return active_ ? recovery_->transactionWalBytes() : 0;
}

} // namespace minidb
