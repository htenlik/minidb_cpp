#include "minidb/recovery.hpp"

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_log.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_lsn.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <limits>
#include <numeric>
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
            || record.type == LogRecordType::PageDeltaUpdate
            || record.type == LogRecordType::PageUpdateV2
            || record.type == LogRecordType::PageDeltaUpdateV2) {
            PageId pageId = INVALID_PAGE_ID;
            bool beforePageExisted = false;
            if (record.type == LogRecordType::PageUpdate) {
                const auto update = decodePageUpdateLogPayload(record.payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            } else if (record.type == LogRecordType::PageDeltaUpdate) {
                const auto update = decodePageDeltaUpdateLogPayload(record.payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            } else if (record.type == LogRecordType::PageUpdateV2) {
                const auto update = decodePageUpdateV2LogPayload(record.payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            } else {
                const auto update = decodePageDeltaUpdateV2LogPayload(record.payload);
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
        bool applied = true;
        if (record->type == LogRecordType::PageUpdate) {
            const auto update = decodePageUpdateLogPayload(record->payload);
            diskManager_.writePhysicalPage(update.pageId, update.afterImage);
            ++stats.legacyRedoRecords;
        } else if (record->type == LogRecordType::PageDeltaUpdate) {
            const auto update = decodePageDeltaUpdateLogPayload(record->payload);
            DiskManager::Page page{};
            if (update.pageId < diskManager_.pageCount()) {
                diskManager_.readPhysicalPage(update.pageId, page);
                ++stats.recoveryPageReads;
            }
            applyPageDeltaAfter(page, update);
            diskManager_.writePhysicalPage(update.pageId, page);
            ++stats.legacyRedoRecords;
        } else {
            PageId pageId = INVALID_PAGE_ID;
            bool beforePageExisted = false;
            if (record->type == LogRecordType::PageUpdateV2) {
                const auto update = decodePageUpdateV2LogPayload(record->payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            } else {
                const auto update = decodePageDeltaUpdateV2LogPayload(record->payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
            }

            DiskManager::Page current{};
            const bool pageExists = pageId < diskManager_.pageCount();
            const bool needsCurrentPage = record->type == LogRecordType::PageDeltaUpdateV2
                || (redoPolicy_ == RedoPolicy::PageLsnSelectiveRedo
                    && pageExists && beforePageExisted);
            if (pageExists && needsCurrentPage) {
                diskManager_.readPhysicalPage(pageId, current);
                ++stats.recoveryPageReads;
            }
            if (redoPolicy_ == RedoPolicy::PageLsnSelectiveRedo
                && pageExists && beforePageExisted) {
                ++stats.pageLsnChecks;
                const auto persistentLsn = readPersistentPageLsn(current);
                if (!isValidLsn(persistentLsn)) {
                    ++stats.pageLsnUnknown;
                } else {
                    if (persistentLsn < wal_file_layout::HEADER_SIZE
                        || persistentLsn >= scan.validBytes) {
                        throw WalError(
                            WalErrorKind::CorruptRecord,
                            "Persistent PageLSN is beyond the known WAL high-water mark");
                    }
                    if (persistentLsn >= record->lsn) {
                        applied = false;
                        ++stats.redoSkippedByPageLsn;
                    }
                }
            }
            if (applied) {
                if (record->type == LogRecordType::PageUpdateV2) {
                    const auto update = decodePageUpdateV2LogPayload(record->payload);
                    current = update.afterImage;
                } else {
                    const auto update = decodePageDeltaUpdateV2LogPayload(record->payload);
                    clearPersistentPageLsn(current);
                    applyPageDeltaAfter(current, PageDeltaUpdateLogPayload{
                        update.pageId, update.beforePageExisted, update.ranges});
                }
                writePersistentPageLsn(current, record->lsn);
                diskManager_.writePhysicalPage(pageId, current);
                if (redoPolicy_ == RedoPolicy::PageLsnSelectiveRedo
                    && pageExists && beforePageExisted) {
                    ++stats.redoAppliedAfterPageLsnCheck;
                }
            }
        }
        stats.databasePagesExtended += diskManager_.pageCount() - beforeCount;
        if (applied) {
            ++stats.databaseWrites;
            ++stats.recoveryPageWrites;
            ++stats.pagesRedone;
            recoveryFailPoint("recovery_after_redo_page");
        }
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
            } else if ((*iterator)->type == LogRecordType::PageDeltaUpdate) {
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
            } else if ((*iterator)->type == LogRecordType::PageUpdateV2) {
                const auto update = decodePageUpdateV2LogPayload((*iterator)->payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
                if (beforePageExisted && restored.insert(pageId).second) {
                    auto before = update.beforeImage;
                    if (supportsPersistentPageLsn(before)) {
                        writePersistentPageLsn(before, update.beforePageLsn);
                    }
                    diskManager_.writePhysicalPage(pageId, before);
                    restoredNow = true;
                }
            } else {
                const auto update = decodePageDeltaUpdateV2LogPayload((*iterator)->payload);
                pageId = update.pageId;
                beforePageExisted = update.beforePageExisted;
                if (beforePageExisted && restored.insert(pageId).second) {
                    DiskManager::Page page{};
                    diskManager_.readPhysicalPage(pageId, page);
                    ++stats.recoveryPageReads;
                    clearPersistentPageLsn(page);
                    applyPageDeltaBefore(page, PageDeltaUpdateLogPayload{
                        update.pageId, update.beforePageExisted, update.ranges});
                    if (supportsPersistentPageLsn(page)) {
                        writePersistentPageLsn(page, update.beforePageLsn);
                    }
                    diskManager_.writePhysicalPage(pageId, page);
                    restoredNow = true;
                }
            }
            if (restoredNow) {
                ++stats.databaseWrites;
                ++stats.recoveryPageWrites;
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
    Lsn beforePageLsn = INVALID_LSN;
    if (existed) {
        beforePageLsn = readPersistentPageLsn(before);
        if (isValidLsn(beforePageLsn)) ++stats_.v2PagesWithKnownLsn;
        else ++stats_.v1PagesObserved;
    }
    active_->pages.emplace(pageId, PageState{
        existed,
        existed ? before : DiskManager::Page{},
        beforePageLsn,
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
    DiskManager::Page& after) {
    if (!active_.has_value()) return INVALID_LSN;
    const auto found = active_->pages.find(pageId);
    if (found == active_->pages.end()) return INVALID_LSN;
    auto& state = found->second;
    auto normalizedAfter = after;
    const bool afterSupportsPageLsn = supportsPersistentPageLsn(normalizedAfter);
    if (afterSupportsPageLsn) clearPersistentPageLsn(normalizedAfter);
    auto normalizedBefore = state.before;
    if (supportsPersistentPageLsn(normalizedBefore)) clearPersistentPageLsn(normalizedBefore);
    const auto& comparison = state.latestAfter.has_value()
        ? *state.latestAfter : normalizedBefore;
    if (comparison == normalizedAfter) {
        if (isValidLsn(state.latestLsn) && afterSupportsPageLsn) {
            writePersistentPageLsn(after, state.latestLsn);
        }
        return state.latestLsn;
    }
    std::uint64_t logicalBytesChanged = 0;
    for (std::size_t offset = 0; offset < normalizedAfter.size(); ++offset) {
        if (comparison[offset] != normalizedAfter[offset]) {
            state.touchedOffsets[offset] = true;
            ++logicalBytesChanged;
        }
    }
    const bool migratingPageZero = pageId == database_format::METADATA_PAGE_ID
        && byte_codec::readUint32(normalizedAfter, database_format::FORMAT_VERSION_OFFSET)
            == database_format::CURRENT_VERSION;
    const bool pageLsnAware = afterSupportsPageLsn
        && (diskManager_.databaseHeader().formatVersion == database_format::CURRENT_VERSION
            || migratingPageZero);
    Lsn lsn = INVALID_LSN;
    std::vector<std::byte> payload;
    LogRecordType recordType = pageLsnAware
        ? LogRecordType::PageUpdateV2 : LogRecordType::PageUpdate;
    if (updateMode_ == WalUpdateMode::FullPage) {
        if (pageLsnAware) {
            payload = encodePageUpdateV2LogPayload(PageUpdateV2LogPayload{
                pageId, state.beforeExisted, state.beforePageLsn,
                normalizedBefore, normalizedAfter,
            });
        } else {
            payload = encodePageUpdateLogPayload(PageUpdateLogPayload{
                pageId, state.beforeExisted, state.before, after,
            });
        }
        stats_.fullPageImageBytes += 2 * database_format::PAGE_SIZE;
        stats_.changedBytes += database_format::PAGE_SIZE;
        ++stats_.fullPageUpdateRecords;
    } else {
        const auto deltaStart = std::chrono::steady_clock::now();
        auto ranges = computePageDelta(
            pageLsnAware ? normalizedBefore : state.before,
            pageLsnAware ? normalizedAfter : after,
            state.touchedOffsets);
        stats_.deltaComputationNs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - deltaStart).count());
        PageDeltaUpdateLogPayload deltaPayload{
            pageId, state.beforeExisted, ranges,
        };
        PageDeltaUpdateV2LogPayload deltaV2Payload{
            pageId, state.beforeExisted, state.beforePageLsn, std::move(ranges),
        };
        const auto& selectedRanges = pageLsnAware
            ? deltaV2Payload.ranges : deltaPayload.ranges;
        const auto changedBytes = std::accumulate(
            selectedRanges.begin(), selectedRanges.end(), std::uint64_t{0},
            [](std::uint64_t sum, const PageByteRange& range) {
                return sum + range.length;
            });
        stats_.rangeCount += selectedRanges.size();
        stats_.deltaRangeCounts.push_back(selectedRanges.size());
        if (updateMode_ == WalUpdateMode::Adaptive) {
            const auto selectionStart = std::chrono::steady_clock::now();
            const auto decision = pageLsnAware
                ? selectAdaptivePageUpdateV2Encoding(deltaV2Payload)
                : selectAdaptivePageUpdateEncoding(deltaPayload);
            stats_.adaptiveSelectionNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - selectionStart).count());
            stats_.bytesIfFullPage += decision.fullPageRecordBytes;
            stats_.bytesIfDelta += decision.deltaRecordBytes;
            const auto chosenBytes = std::min(
                decision.fullPageRecordBytes, decision.deltaRecordBytes);
            stats_.bytesActuallyChosen += chosenBytes;
            stats_.bytesSavedByAdaptive +=
                decision.fullPageRecordBytes - chosenBytes;
            stats_.bytesSavedVersusByteRange +=
                decision.deltaRecordBytes - chosenBytes;
            if (decision.isTie()) ++stats_.adaptiveTies;
            recordType = decision.recordType;
            if (recordType == LogRecordType::PageUpdate
                || recordType == LogRecordType::PageUpdateV2) {
                ++stats_.adaptiveFullPageSelections;
                if (pageLsnAware) {
                    payload = encodePageUpdateV2LogPayload(PageUpdateV2LogPayload{
                        pageId, state.beforeExisted, state.beforePageLsn,
                        normalizedBefore, normalizedAfter,
                    });
                } else {
                    payload = encodePageUpdateLogPayload(PageUpdateLogPayload{
                        pageId, state.beforeExisted, state.before, after,
                    });
                }
                stats_.fullPageImageBytes += 2 * database_format::PAGE_SIZE;
                stats_.changedBytes += database_format::PAGE_SIZE;
                ++stats_.fullPageUpdateRecords;
            } else {
                ++stats_.adaptiveDeltaSelections;
                payload = pageLsnAware
                    ? encodePageDeltaUpdateV2LogPayload(deltaV2Payload)
                    : encodePageDeltaUpdateLogPayload(deltaPayload);
                stats_.changedBytes += changedBytes;
                ++stats_.byteRangeUpdateRecords;
            }
        } else {
            recordType = pageLsnAware
                ? LogRecordType::PageDeltaUpdateV2 : LogRecordType::PageDeltaUpdate;
            payload = pageLsnAware
                ? encodePageDeltaUpdateV2LogPayload(deltaV2Payload)
                : encodePageDeltaUpdateLogPayload(deltaPayload);
            stats_.changedBytes += changedBytes;
            ++stats_.byteRangeUpdateRecords;
        }
    }
    const auto payloadBytes = payload.size();
    lsn = appendTransactionRecord(recordType, std::move(payload));
    stats_.logicalBytesChanged += logicalBytesChanged;
    stats_.walUpdatePayloadBytes += payloadBytes;
    stats_.updateRecordBytes.push_back(wal_record_layout::HEADER_SIZE + payloadBytes);
    state.latestAfter = normalizedAfter;
    state.latestLsn = lsn;
    if (pageLsnAware) {
        writePersistentPageLsn(after, lsn);
        ++stats_.persistentPageLsnAssignments;
    }
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
            bufferPool_->prepareResidentPageForCommit(pageId);
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
