#include "minidb/recovery.hpp"

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_log.hpp"
#include "minidb/log_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace minidb {
namespace {

enum class TransactionStatus { Active, Committed, Aborted };

struct AnalyzedTransaction {
    TransactionId id = INVALID_TRANSACTION_ID;
    TransactionStatus status = TransactionStatus::Active;
    BeginLogPayload begin{};
    Lsn lastLsn = INVALID_LSN;
    std::vector<const LogRecord*> updates;
};

TransactionId nextTransactionIdFrom(const WalScanResult& scan) {
    TransactionId maximum = 0;
    for (const auto& record : scan.records) maximum = std::max(maximum, record.transactionId);
    if (maximum == std::numeric_limits<TransactionId>::max()) {
        throw std::overflow_error("WAL transaction ID space is exhausted");
    }
    return maximum + 1;
}

} // namespace

void recoveryFailPoint(std::string_view name) {
    const auto* throwing = std::getenv("MINIDB_THROWPOINT");
    if (throwing != nullptr && name == throwing) {
        throw std::runtime_error("Injected failure at " + std::string(name));
    }
    const auto* configured = std::getenv("MINIDB_FAILPOINT");
    if (configured != nullptr && name == configured) ::_exit(86);
}

RecoveryStats RecoveryManager::recover() {
    const auto totalStart = std::chrono::steady_clock::now();
    RecoveryStats stats;
    CheckpointSelection checkpoint;
    if (checkpointControl_ != nullptr && !forceFullScan_) {
        checkpoint = checkpointControl_->select(logManager_);
        stats.checkpointControlPresent = checkpoint.controlFilePresent;
        stats.checkpointValidationFailures = checkpoint.validationFailures;
    } else if (checkpointControl_ != nullptr) {
        stats.checkpointControlPresent = std::filesystem::exists(checkpointControl_->path());
    }
    const auto analysisStart = std::chrono::steady_clock::now();
    if (checkpoint.slot.has_value()) {
        stats.checkpointUsed = true;
        stats.fullScanFallback = false;
        stats.checkpointId = checkpoint.slot->checkpointId;
        stats.checkpointEndLsn = checkpoint.slot->checkpointEndLsn;
        stats.checkpointGeneration = checkpoint.slot->generation;
        stats.recoveryStartOffset = checkpoint.slot->recoveryStartOffset;
        stats.walBytesSkipped = checkpoint.slot->recoveryStartOffset
            - wal_file_layout::HEADER_SIZE;
        stats.highestCheckpointId = checkpoint.slot->checkpointId;
    }
    auto scan = logManager_.scanFrom(stats.recoveryStartOffset);
    stats.walBytesScanned = scan.fileBytes - stats.recoveryStartOffset;
    if (scan.truncatedTail) {
        stats.repairedTail = true;
        stats.tailBytesTruncated = scan.fileBytes - scan.validBytes;
    }
    if (scan.truncatedTail && !logManager_.recoveryPending()) {
        logManager_.truncateToLastValidRecord();
    } else {
        logManager_.completeRecoveryScan(scan, stats.checkpointEndLsn);
    }
    scan.truncatedTail = false;
    scan.fileBytes = scan.validBytes;
    std::map<TransactionId, AnalyzedTransaction> transactions;
    std::optional<TransactionId> activeTransaction;
    std::map<CheckpointId, CheckpointBeginLogPayload> checkpointBegins;
    TransactionId highestTailTransactionId = 0;
    for (const auto& record : scan.records) {
        ++stats.recordsAnalyzed;
        if (record.type == LogRecordType::CheckpointBegin
            || record.type == LogRecordType::CheckpointEnd) {
            validateCheckpointRecord(record);
            if (activeTransaction.has_value()) {
                throw WalError(WalErrorKind::CorruptRecord,
                               "Checkpoint record overlaps an active transaction");
            }
            if (record.type == LogRecordType::CheckpointBegin) {
                const auto payload = decodeCheckpointBeginLogPayload(record.payload);
                checkpointBegins[payload.checkpointId] = payload;
                stats.highestCheckpointId = std::max(stats.highestCheckpointId,
                                                      payload.checkpointId);
            } else {
                const auto payload = decodeCheckpointEndLogPayload(record.payload);
                const auto foundBegin = checkpointBegins.find(payload.checkpointId);
                if (foundBegin == checkpointBegins.end()
                    || payload.checkpointBeginLsn != foundBegin->second.walStartOffset
                    || payload.recoveryStartOffset
                        != record.lsn + wal_record_layout::HEADER_SIZE + record.payload.size()) {
                    throw WalError(WalErrorKind::CorruptRecord,
                                   "CHECKPOINT_END does not match a preceding BEGIN");
                }
                stats.highestCheckpointId = std::max(stats.highestCheckpointId,
                                                      payload.checkpointId);
            }
            continue;
        }
        validateTransactionRecordPayload(record);
        highestTailTransactionId = std::max(highestTailTransactionId, record.transactionId);
        auto found = transactions.find(record.transactionId);
        if (record.type == LogRecordType::Begin) {
            if (found != transactions.end() || activeTransaction.has_value()) {
                throw WalError(WalErrorKind::CorruptRecord, "Transaction has duplicate BEGIN records");
            }
            AnalyzedTransaction transaction;
            transaction.id = record.transactionId;
            transaction.begin = decodeBeginLogPayload(record.payload);
            transaction.lastLsn = record.lsn;
            transactions.emplace(record.transactionId, std::move(transaction));
            activeTransaction = record.transactionId;
            continue;
        }
        if (!activeTransaction.has_value() || *activeTransaction != record.transactionId
            || found == transactions.end() || found->second.status != TransactionStatus::Active
            || record.prevLsn != found->second.lastLsn) {
            throw WalError(WalErrorKind::CorruptRecord, "WAL transaction chain is malformed");
        }
        if (record.type == LogRecordType::PageUpdate) {
            found->second.updates.push_back(&record);
        } else if (record.type == LogRecordType::Commit) {
            found->second.status = TransactionStatus::Committed;
            ++stats.committedTransactions;
            activeTransaction.reset();
        } else if (record.type == LogRecordType::Abort) {
            found->second.status = TransactionStatus::Aborted;
            ++stats.abortedTransactions;
            activeTransaction.reset();
        }
        found->second.lastLsn = record.lsn;
    }
    stats.transactionsAnalyzed = transactions.size();
    TransactionId baseNext = checkpoint.slot.has_value()
        ? checkpoint.slot->nextTransactionId : TransactionId{1};
    if (highestTailTransactionId == std::numeric_limits<TransactionId>::max()) {
        throw std::overflow_error("WAL transaction ID space is exhausted");
    }
    stats.nextTransactionId = std::max(baseNext, highestTailTransactionId + 1);

    std::vector<const LogRecord*> redo;
    AnalyzedTransaction* loser = nullptr;
    for (auto& [id, transaction] : transactions) {
        static_cast<void>(id);
        if (transaction.status == TransactionStatus::Committed) {
            redo.insert(redo.end(), transaction.updates.begin(), transaction.updates.end());
        } else if (transaction.status == TransactionStatus::Active) {
            if (loser != nullptr) {
                throw WalError(WalErrorKind::CorruptRecord, "WAL contains multiple active transactions");
            }
            loser = &transaction;
            ++stats.loserTransactions;
        }
    }
    std::sort(redo.begin(), redo.end(), [](const LogRecord* left, const LogRecord* right) {
        return left->lsn < right->lsn;
    });
    stats.analysisNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - analysisStart).count());
    const auto redoStart = std::chrono::steady_clock::now();
    for (const auto* record : redo) {
        const auto update = decodePageUpdateLogPayload(record->payload);
        const auto beforeCount = diskManager_.pageCount();
        diskManager_.writePhysicalPage(update.pageId, update.afterImage);
        stats.databasePagesExtended += diskManager_.pageCount() - beforeCount;
        ++stats.databaseWrites;
        ++stats.pagesRedone;
        recoveryFailPoint("recovery_after_redo_page");
    }
    stats.redoNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - redoStart).count());

    const auto undoStart = std::chrono::steady_clock::now();
    if (loser != nullptr) {
        std::unordered_set<PageId> restored;
        for (auto iterator = loser->updates.rbegin(); iterator != loser->updates.rend(); ++iterator) {
            const auto update = decodePageUpdateLogPayload((*iterator)->payload);
            if (update.beforePageExisted && restored.insert(update.pageId).second) {
                diskManager_.writePhysicalPage(update.pageId, update.beforeImage);
                ++stats.databaseWrites;
                ++stats.pagesUndone;
                recoveryFailPoint("recovery_after_undo_page");
            }
        }
        const auto beforeCount = diskManager_.pageCount();
        if (beforeCount > loser->begin.startPageCount) {
            diskManager_.truncateToPageCount(loser->begin.startPageCount);
            stats.pagesTruncated = beforeCount - loser->begin.startPageCount;
        }
    }
    stats.undoNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - undoStart).count());
    diskManager_.sync();
    ++stats.databaseSyncCalls;
    recoveryFailPoint("recovery_after_database_sync");

    if (loser != nullptr) {
        const auto abortLsn = logManager_.append(LogRecord{
            LogRecordType::Abort, loser->id, loser->lastLsn, {}, INVALID_LSN,
        });
        logManager_.flushUpTo(abortLsn);
        ++stats.abortedTransactions;
        recoveryFailPoint("recovery_after_abort_sync");
    }
    diskManager_.reloadDatabaseHeader();
    stats.totalNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - totalStart).count());
    return stats;
}

RecoveryCoordinator::RecoveryCoordinator(
    DiskManager& diskManager,
    LogManager& logManager,
    TransactionId nextTransactionId)
    : diskManager_(diskManager),
      logManager_(logManager),
      nextTransactionId_(nextTransactionId == INVALID_TRANSACTION_ID
          ? nextTransactionIdFrom(logManager.scan()) : nextTransactionId) {}

void RecoveryCoordinator::attachBufferPool(BufferPoolManager& bufferPool) noexcept {
    bufferPool_ = &bufferPool;
}

TransactionId RecoveryCoordinator::activeTransactionId() const noexcept {
    return active_.has_value() ? active_->transactionId : INVALID_TRANSACTION_ID;
}

void RecoveryCoordinator::beginStatement() {
    if (active_.has_value()) throw std::logic_error("A mutating statement is already active");
    if (nextTransactionId_ == INVALID_TRANSACTION_ID) {
        throw std::overflow_error("Transaction ID space is exhausted");
    }
    active_.emplace(ActiveStatement{
        nextTransactionId_++, diskManager_.pageCount(), INVALID_LSN, INVALID_LSN, {},
    });
    ++stats_.transactionsBegun;
}

void RecoveryCoordinator::notePageWriteIntent(
    PageId pageId,
    const DiskManager::Page& before) {
    if (!active_.has_value()) {
        throw std::logic_error("Page mutation requires an active statement transaction");
    }
    if (pageId == INVALID_PAGE_ID) throw std::invalid_argument("Write intent has invalid PageId");
    if (active_->pages.contains(pageId)) return;
    const bool existed = pageId < active_->startPageCount;
    active_->pages.emplace(pageId, PageState{
        existed,
        existed ? before : DiskManager::Page{},
        std::nullopt,
        INVALID_LSN,
    });
    ++stats_.pagesFirstWritten;
}

void RecoveryCoordinator::ensureBeginLogged() {
    if (isValidLsn(active_->beginLsn)) return;
    active_->beginLsn = logManager_.append(LogRecord{
        LogRecordType::Begin,
        active_->transactionId,
        INVALID_LSN,
        encodeBeginLogPayload(BeginLogPayload{active_->startPageCount}),
        INVALID_LSN,
    });
    active_->previousLsn = active_->beginLsn;
    recoveryFailPoint("after_begin_append");
}

Lsn RecoveryCoordinator::appendTransactionRecord(
    LogRecordType type,
    std::vector<std::byte> payload) {
    ensureBeginLogged();
    const auto lsn = logManager_.append(LogRecord{
        type, active_->transactionId, active_->previousLsn, std::move(payload), INVALID_LSN,
    });
    active_->previousLsn = lsn;
    return lsn;
}

Lsn RecoveryCoordinator::preparePageForWrite(
    PageId pageId,
    const DiskManager::Page& after) {
    if (!active_.has_value()) return INVALID_LSN;
    const auto found = active_->pages.find(pageId);
    if (found == active_->pages.end()) return INVALID_LSN;
    auto& state = found->second;
    const auto& comparison = state.latestAfter.has_value() ? *state.latestAfter : state.before;
    if (comparison == after) return state.latestLsn;
    const auto lsn = appendTransactionRecord(
        LogRecordType::PageUpdate,
        encodePageUpdateLogPayload(PageUpdateLogPayload{
            pageId, state.beforeExisted, state.before, after,
        }));
    state.latestAfter = after;
    state.latestLsn = lsn;
    ++stats_.pageUpdateRecords;
    stats_.fullPageImageBytes += 2 * database_format::PAGE_SIZE;
    recoveryFailPoint("after_page_update_append");
    return lsn;
}

void RecoveryCoordinator::requireNoPins() const {
    if (bufferPool_ != nullptr && bufferPool_->totalPinCount() != 0) {
        throw std::logic_error("Statement completion requires all page guards to be released");
    }
}

void RecoveryCoordinator::commitStatement() {
    if (!active_.has_value()) throw std::logic_error("No statement transaction is active");
    requireNoPins();
    if (bufferPool_ != nullptr) {
        for (auto& [pageId, state] : active_->pages) {
            static_cast<void>(state);
            if (const auto page = bufferPool_->residentPageCopy(pageId); page.has_value()) {
                static_cast<void>(preparePageForWrite(pageId, *page));
            }
        }
    }
    if (!isValidLsn(active_->beginLsn)) {
        ++stats_.zeroWriteTransactions;
        ++stats_.transactionsCommitted;
        active_.reset();
        return;
    }
    recoveryFailPoint("before_commit_append");
    const auto commitLsn = appendTransactionRecord(LogRecordType::Commit);
    recoveryFailPoint("after_commit_append");
    logManager_.flushUpTo(commitLsn);
    ++stats_.commitFsyncs;
    recoveryFailPoint("after_commit_sync");
    active_.reset();
    ++stats_.transactionsCommitted;
}

void RecoveryCoordinator::rollbackStatement() {
    if (!active_.has_value()) throw std::logic_error("No statement transaction is active");
    requireNoPins();
    rollbackActive_ = true;
    const auto startPageCount = active_->startPageCount;
    const auto transactionId = active_->transactionId;
    const auto previousLsn = active_->previousLsn;
    const bool logged = isValidLsn(active_->beginLsn);
    if (bufferPool_ != nullptr) {
        for (const auto& [pageId, state] : active_->pages) {
            static_cast<void>(state);
            bufferPool_->discardPageForRecovery(pageId);
        }
        bufferPool_->discardPagesAtOrAboveForRecovery(static_cast<PageId>(startPageCount));
    }
    for (const auto& [pageId, state] : active_->pages) {
        if (state.beforeExisted) {
            diskManager_.writePhysicalPage(pageId, state.before);
            ++stats_.rollbackDatabaseWrites;
        }
    }
    if (diskManager_.pageCount() > startPageCount) {
        diskManager_.truncateToPageCount(startPageCount);
    }
    diskManager_.sync();
    recoveryFailPoint("rollback_after_database_sync");
    active_.reset();
    if (logged) {
        const auto abortLsn = logManager_.append(LogRecord{
            LogRecordType::Abort, transactionId, previousLsn, {}, INVALID_LSN,
        });
        logManager_.flushUpTo(abortLsn);
        recoveryFailPoint("rollback_after_abort_sync");
    }
    diskManager_.reloadDatabaseHeader();
    rollbackActive_ = false;
    ++stats_.transactionsRolledBack;
}

} // namespace minidb
