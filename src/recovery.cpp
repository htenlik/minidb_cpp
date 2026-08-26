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

std::optional<CheckpointSlot> discoverRetainedCheckpoint(const LogManager& logManager) {
    if (!logManager.isSegmented()) return std::nullopt;
    const auto scan = logManager.scan();
    std::map<CheckpointId, std::pair<Lsn, CheckpointBeginLogPayload>> begins;
    std::optional<CheckpointSlot> newest;
    for (const auto& record : scan.records) {
        if (record.type == LogRecordType::CheckpointBegin) {
            validateCheckpointRecord(record);
            const auto payload = decodeCheckpointBeginLogPayload(record.payload);
            begins[payload.checkpointId] = {record.lsn, payload};
            continue;
        }
        if (record.type != LogRecordType::CheckpointEnd) continue;
        validateCheckpointRecord(record);
        const auto payload = decodeCheckpointEndLogPayload(record.payload);
        const auto begin = begins.find(payload.checkpointId);
        const auto recordEnd = record.lsn + wal_record_layout::HEADER_SIZE
            + record.payload.size();
        if (begin == begins.end()
            || begin->second.first != payload.checkpointBeginLsn
            || payload.recoveryStartOffset != recordEnd) {
            continue;
        }
        if (!newest.has_value() || payload.checkpointId > newest->checkpointId) {
            newest = CheckpointSlot{
                1,
                payload.checkpointId,
                record.lsn,
                payload.recoveryStartOffset,
                payload.databasePageCount,
                payload.nextTransactionId,
                payload.recoveryStartOffset,
            };
        }
    }
    return newest;
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
    stats.recoveryStartOffset = logManager_.oldestRetainedLsn();
    CheckpointSelection checkpoint;
    if (checkpointControl_ != nullptr && !forceFullScan_) {
        checkpoint = checkpointControl_->select(logManager_);
        stats.checkpointControlPresent = checkpoint.controlFilePresent;
        stats.checkpointValidationFailures = checkpoint.validationFailures;
        if (!checkpoint.slot.has_value()) {
            checkpoint.slot = discoverRetainedCheckpoint(logManager_);
            if (checkpoint.slot.has_value()) {
                try {
                    checkpointControl_->publish(*checkpoint.slot);
                } catch (const std::exception&) {
                    // The WAL checkpoint remains authoritative; control rebuild is
                    // an optimization and must not turn recoverable state into failure.
                }
            }
        }
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
        if (record.type == LogRecordType::PageUpdate
            || record.type == LogRecordType::PageDeltaUpdate) {
            PageId pageId = INVALID_PAGE_ID;
            bool beforePageExisted = false;
            if (record.type == LogRecordType::PageUpdate) {
                const auto update = decodePageUpdateLogPayload(record.payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            } else {
                const auto update = decodePageDeltaUpdateLogPayload(record.payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            }
            if (beforePageExisted != (pageId < found->second.begin.startPageCount)) {
                throw WalError(WalErrorKind::CorruptRecord,
                               "Page-update existence flag contradicts transaction BEGIN");
            }
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
        const auto beforeCount = diskManager_.pageCount();
        if (record->type == LogRecordType::PageUpdate) {
            const auto update = decodePageUpdateLogPayload(record->payload);
            diskManager_.writePhysicalPage(update.pageId, update.afterImage);
        } else {
            const auto update = decodePageDeltaUpdateLogPayload(record->payload);
            DiskManager::Page page{};
            if (update.pageId < diskManager_.pageCount()) {
                diskManager_.readPhysicalPage(update.pageId, page);
            }
            applyPageDeltaAfter(page, update);
            diskManager_.writePhysicalPage(update.pageId, page);
        }
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
            PageId pageId = INVALID_PAGE_ID;
            bool beforePageExisted = false;
            bool restoredNow = false;
            if ((*iterator)->type == LogRecordType::PageUpdate) {
                const auto update = decodePageUpdateLogPayload((*iterator)->payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
                if (beforePageExisted && restored.insert(pageId).second) {
                    diskManager_.writePhysicalPage(pageId, update.beforeImage);
                    restoredNow = true;
                }
            } else {
                const auto update = decodePageDeltaUpdateLogPayload((*iterator)->payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
                if (beforePageExisted && restored.insert(pageId).second) {
                    DiskManager::Page page{};
                    diskManager_.readPhysicalPage(pageId, page);
                    applyPageDeltaBefore(page, update);
                    diskManager_.writePhysicalPage(pageId, page);
                    restoredNow = true;
                }
            }
            if (restoredNow) {
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
    TransactionId nextTransactionId,
    WalUpdateMode updateMode)
    : diskManager_(diskManager),
      logManager_(logManager),
      nextTransactionId_(nextTransactionId == INVALID_TRANSACTION_ID
          ? nextTransactionIdFrom(logManager.scan()) : nextTransactionId),
      updateMode_(updateMode) {}

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
        {},
        INVALID_LSN,
    });
    ++stats_.pagesFirstWritten;
}

void RecoveryCoordinator::ensureBeginLogged() {
    if (isValidLsn(active_->beginLsn)) return;
    const auto payload = encodeBeginLogPayload(BeginLogPayload{active_->startPageCount});
    active_->beginLsn = logManager_.append(LogRecord{
        LogRecordType::Begin,
        active_->transactionId,
        INVALID_LSN,
        payload,
        INVALID_LSN,
    });
    stats_.walTotalBytesGenerated += wal_record_layout::HEADER_SIZE + payload.size();
    active_->previousLsn = active_->beginLsn;
    recoveryFailPoint("after_begin_append");
}

Lsn RecoveryCoordinator::appendTransactionRecord(
    LogRecordType type,
    std::vector<std::byte> payload) {
    ensureBeginLogged();
    const auto recordBytes = wal_record_layout::HEADER_SIZE + payload.size();
    const auto lsn = logManager_.append(LogRecord{
        type, active_->transactionId, active_->previousLsn, std::move(payload), INVALID_LSN,
    });
    stats_.walTotalBytesGenerated += recordBytes;
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
    std::uint64_t logicalBytesChanged = 0;
    for (std::size_t offset = 0; offset < after.size(); ++offset) {
        if (comparison[offset] != after[offset]) {
            state.touchedOffsets[offset] = true;
            ++logicalBytesChanged;
        }
    }
    Lsn lsn = INVALID_LSN;
    std::vector<std::byte> payload;
    if (updateMode_ == WalUpdateMode::FullPage) {
        payload = encodePageUpdateLogPayload(PageUpdateLogPayload{
            pageId, state.beforeExisted, state.before, after,
        });
        stats_.fullPageImageBytes += 2 * database_format::PAGE_SIZE;
        stats_.changedBytes += database_format::PAGE_SIZE;
        ++stats_.fullPageUpdateRecords;
    } else {
        const auto deltaStart = std::chrono::steady_clock::now();
        auto ranges = computePageDelta(state.before, after, state.touchedOffsets);
        stats_.deltaComputationNs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - deltaStart).count());
        for (const auto& range : ranges) stats_.changedBytes += range.length;
        stats_.rangeCount += ranges.size();
        stats_.deltaRangeCounts.push_back(ranges.size());
        payload = encodePageDeltaUpdateLogPayload(PageDeltaUpdateLogPayload{
            pageId, state.beforeExisted, std::move(ranges),
        });
        ++stats_.byteRangeUpdateRecords;
    }
    const auto payloadBytes = payload.size();
    const auto recordType = updateMode_ == WalUpdateMode::FullPage
        ? LogRecordType::PageUpdate : LogRecordType::PageDeltaUpdate;
    lsn = appendTransactionRecord(recordType, std::move(payload));
    stats_.logicalBytesChanged += logicalBytesChanged;
    stats_.walUpdatePayloadBytes += payloadBytes;
    stats_.updateRecordBytes.push_back(wal_record_layout::HEADER_SIZE + payloadBytes);
    state.latestAfter = after;
    state.latestLsn = lsn;
    ++stats_.pageUpdateRecords;
    ++stats_.updateRecordCount;
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
        stats_.walTotalBytesGenerated += wal_record_layout::HEADER_SIZE;
        logManager_.flushUpTo(abortLsn);
        recoveryFailPoint("rollback_after_abort_sync");
    }
    diskManager_.reloadDatabaseHeader();
    rollbackActive_ = false;
    ++stats_.transactionsRolledBack;
}

} // namespace minidb
