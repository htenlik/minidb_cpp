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
#include <functional>
#include <map>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace minidb {
namespace {

using TransactionStatus = RecoveryTransactionStatus;

struct AnalyzedTransaction : RecoveryTransactionEntry {
    std::vector<const LogRecord*> updates;
    std::vector<const LogRecord*> clrs;
};

struct PageUpdateIdentity {
    PageId pageId = INVALID_PAGE_ID;
    bool beforePageExisted = false;
};

PageUpdateIdentity pageUpdateIdentity(const LogRecord& record) {
    if (record.type == LogRecordType::Compensation) {
        const auto compensation = decodeCompensationLogPayload(record.payload);
        return {compensation.pageId, compensation.pageExisted};
    }
    if (record.type == LogRecordType::PageUpdate) {
        const auto update = decodePageUpdateLogPayload(record.payload);
        return {update.pageId, update.beforePageExisted};
    }
    if (record.type == LogRecordType::PageDeltaUpdate) {
        const auto update = decodePageDeltaUpdateLogPayload(record.payload);
        return {update.pageId, update.beforePageExisted};
    }
    if (record.type == LogRecordType::PageUpdateV2) {
        const auto update = decodePageUpdateV2LogPayload(record.payload);
        return {update.pageId, update.beforePageExisted};
    }
    if (record.type == LogRecordType::PageDeltaUpdateV2) {
        const auto update = decodePageDeltaUpdateV2LogPayload(record.payload);
        return {update.pageId, update.beforePageExisted};
    }
    throw WalError(WalErrorKind::CorruptRecord,
                   "WAL record is not a page-update record");
}

bool isPageUpdateRecord(LogRecordType type) noexcept {
    return type == LogRecordType::PageUpdate
        || type == LogRecordType::PageDeltaUpdate
        || type == LogRecordType::PageUpdateV2
        || type == LogRecordType::PageDeltaUpdateV2;
}

bool isPageAffectingRecord(LogRecordType type) noexcept {
    return isPageUpdateRecord(type) || type == LogRecordType::Compensation;
}

DiskManager::Page compensatedPageFor(
    const LogRecord& record,
    const std::function<void(PageId, DiskManager::Page&)>& readUndoBase) {
    DiskManager::Page compensated{};
    if (record.type == LogRecordType::PageUpdate) {
        compensated = decodePageUpdateLogPayload(record.payload).beforeImage;
    } else if (record.type == LogRecordType::PageUpdateV2) {
        compensated = decodePageUpdateV2LogPayload(record.payload).beforeImage;
    } else if (record.type == LogRecordType::PageDeltaUpdate
               || record.type == LogRecordType::PageDeltaUpdateV2) {
        const auto identity = pageUpdateIdentity(record);
        readUndoBase(identity.pageId, compensated);
        if (record.type == LogRecordType::PageDeltaUpdate) {
            applyPageDeltaBefore(
                compensated, decodePageDeltaUpdateLogPayload(record.payload));
        } else {
            const auto update = decodePageDeltaUpdateV2LogPayload(record.payload);
            clearPersistentPageLsn(compensated);
            applyPageDeltaBefore(compensated, PageDeltaUpdateLogPayload{
                update.pageId, update.beforePageExisted, update.ranges});
        }
    } else {
        throw WalError(WalErrorKind::CorruptRecord,
                       "UNDO target is not a page-update record");
    }
    if (supportsPersistentPageLsn(compensated)) clearPersistentPageLsn(compensated);
    return compensated;
}

// Both startup loser recovery and live transaction rollback use this one WAL
// traversal/durability protocol. Live rollback supplies its retained original
// page as the delta base: disk may not yet contain an earlier NO-FORCE winner.
void undoTransactions(
    DiskManager& disk,
    LogManager& log,
    std::vector<RecoveryTransactionEntry*> losers,
    const std::function<LogRecord(Lsn)>& readRecord,
    const std::function<void(PageId, DiskManager::Page&)>& readUndoBase,
    RecoveryStats& stats,
    bool liveRollback = false) {
    const auto undoStart = std::chrono::steady_clock::now();
    using Work = std::pair<Lsn, std::size_t>;
    std::priority_queue<Work> work;
    for (std::size_t i = 0; i < losers.size(); ++i) {
        losers[i]->status = TransactionStatus::Aborting;
        losers[i]->undoNextLsn = losers[i]->lastLsn;
        work.emplace(losers[i]->lastLsn, i);
    }
    stats.undoQueuePeak = work.size();
    while (!work.empty()) {
        const auto [position, index] = work.top();
        work.pop();
        ++stats.undoQueuePops;
        auto& transaction = *losers[index];
        const auto transactionId = transaction.transactionId;
        auto& transactionLastLsn = transaction.lastLsn;
        auto& nextUndoLsn = transaction.undoNextLsn;
        const auto startPageCount = transaction.startPageCount;
        nextUndoLsn = position;
        const auto record = readRecord(nextUndoLsn);
        if (record.transactionId != transactionId) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "UNDO chain crosses transaction ownership");
        }
        if (record.type == LogRecordType::Begin) {
            if (transaction.clrCount != 0) recoveryFailPoint("recovery_after_final_clr");
            const auto beforeCount = disk.pageCount();
            if (beforeCount > startPageCount) {
                disk.truncateToPageCount(startPageCount);
                stats.pagesTruncated += beforeCount - startPageCount;
                disk.sync();
                ++stats.databaseSyncCalls;
                recoveryFailPoint("recovery_after_appended_page_truncation");
            }
            stats.loserLastLsn = transactionLastLsn;
            stats.loserLastUndoNextLsn = nextUndoLsn;
            disk.sync();
            ++stats.databaseSyncCalls;
            recoveryFailPoint("recovery_after_database_sync");
            if (liveRollback) recoveryFailPoint("rollback_after_database_sync");
            recoveryFailPoint("recovery_before_abort_append");
            const auto abortLsn = log.append(LogRecord{
                LogRecordType::Abort, transactionId, transactionLastLsn, {}, INVALID_LSN,
            });
            recoveryFailPoint("recovery_after_abort_append");
            log.flushUpTo(abortLsn);
            transactionLastLsn = abortLsn;
            transaction.undoNextLsn = INVALID_LSN;
            transaction.status = TransactionStatus::Aborted;
            transaction.hasDurableAbort = true;
            ++stats.abortedTransactions;
            stats.durableAbortObserved = true;
            recoveryFailPoint("recovery_after_abort_fsync");
            recoveryFailPoint("recovery_after_abort_sync");
            if (liveRollback) recoveryFailPoint("rollback_after_abort_sync");
            continue;
        }
        if (record.type == LogRecordType::Compensation) {
            nextUndoLsn = decodeCompensationLogPayload(record.payload).undoNextLsn;
            ++stats.undoClrsEncountered;
            ++stats.undoRecordsSkippedByClr;
            work.emplace(nextUndoLsn, index);
            continue;
        }
        if (!isPageUpdateRecord(record.type)) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "UNDO chain contains a non-undoable record");
        }
        ++stats.undoUserRecordsVisited;
        const auto identity = pageUpdateIdentity(record);
        nextUndoLsn = record.prevLsn;
        // Appended pages are removed by the idempotent final truncation.
        if (!identity.beforePageExisted) {
            work.emplace(nextUndoLsn, index);
            continue;
        }

        auto compensated = compensatedPageFor(record, readUndoBase);
        const bool pageSupportsLsn = supportsPersistentPageLsn(compensated);
        recoveryFailPoint("recovery_before_clr_append");
        const auto payload = encodeCompensationLogPayload(CompensationLogPayload{
            identity.pageId, true, pageSupportsLsn, nextUndoLsn, record.lsn, compensated,
        });
        const auto clrLsn = log.append(LogRecord{
            LogRecordType::Compensation, transactionId, transactionLastLsn,
            payload, INVALID_LSN,
        });
        transactionLastLsn = clrLsn;
        ++transaction.clrCount;
        ++stats.clrsAppended;
        ++stats.undoUserRecordsCompensated;
        stats.undoWalBytes += wal_record_layout::HEADER_SIZE + payload.size();
        recoveryFailPoint("recovery_after_clr_append");
        log.flushUpTo(clrLsn);
        recoveryFailPoint("recovery_after_clr_wal_force");
        if (pageSupportsLsn) writePersistentPageLsn(compensated, clrLsn);
        disk.writePhysicalPage(identity.pageId, compensated);
        disk.sync();
        ++stats.databaseSyncCalls;
        ++stats.databaseWrites;
        ++stats.recoveryPageWrites;
        ++stats.pagesUndone;
        ++stats.undoPageWrites;
        recoveryFailPoint("recovery_after_compensation_page_write");
        recoveryFailPoint("recovery_after_undo_page");
        recoveryFailPoint("recovery_midway_loser_chain");
        const auto* crashAfter = std::getenv("MINIDB_RECOVERY_CRASH_AFTER_CLRS");
        if (crashAfter != nullptr && stats.clrsAppended == std::strtoull(crashAfter, nullptr, 10)) {
            ::_exit(86);
        }
        work.emplace(nextUndoLsn, index);
    }
    stats.undoNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - undoStart).count());
}

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
    std::map<CheckpointId, Lsn> fuzzyBegins;
    std::optional<CheckpointSlot> newest;
    for (const auto& record : scan.records) {
        if (record.type == LogRecordType::CheckpointBegin) {
            validateCheckpointRecord(record);
            const auto payload = decodeCheckpointBeginLogPayload(record.payload);
            begins[payload.checkpointId] = {record.lsn, payload};
            continue;
        }
        if (record.type == LogRecordType::FuzzyCheckpointBegin) {
            validateCheckpointRecord(record);
            const auto payload = decodeFuzzyCheckpointBeginLogPayload(record.payload);
            fuzzyBegins[payload.checkpointId] = record.lsn;
            continue;
        }
        if (record.type == LogRecordType::FuzzyCheckpointEnd) {
            validateCheckpointRecord(record);
            const auto payload = decodeFuzzyCheckpointEndLogPayload(record.payload);
            const auto begin = fuzzyBegins.find(payload.checkpointId);
            if (begin == fuzzyBegins.end() || begin->second != payload.checkpointBeginLsn) {
                continue;
            }
            const auto recordEnd = record.lsn + wal_record_layout::HEADER_SIZE
                + record.payload.size();
            if (!newest.has_value() || payload.checkpointId > newest->checkpointId) {
                newest = CheckpointSlot{
                    1,
                    payload.checkpointId,
                    record.lsn,
                    payload.checkpointBeginLsn,
                    payload.databasePageCount,
                    payload.nextTransactionId,
                    recordEnd,
                };
            }
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
    analysisTransactions_.clear();
    recoveredTransactions_.clear();
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
    std::map<PageId, Lsn> dirtyPageTable;
    std::optional<FuzzyCheckpointEndLogPayload> fuzzyCheckpoint;
    if (checkpoint.slot.has_value()) {
        stats.checkpointUsed = true;
        stats.fullScanFallback = false;
        stats.checkpointId = checkpoint.slot->checkpointId;
        stats.checkpointEndLsn = checkpoint.slot->checkpointEndLsn;
        stats.checkpointWalHighWater = checkpoint.slot->walFileSizeAtCheckpoint;
        stats.checkpointGeneration = checkpoint.slot->generation;
        stats.recoveryStartOffset = checkpoint.slot->recoveryStartOffset;
        stats.walBytesSkipped = checkpoint.slot->recoveryStartOffset
            - wal_file_layout::HEADER_SIZE;
        stats.highestCheckpointId = checkpoint.slot->checkpointId;
        const auto endRecord = logManager_.readRecordAt(checkpoint.slot->checkpointEndLsn);
        if (endRecord.type == LogRecordType::FuzzyCheckpointEnd) {
            validateCheckpointRecord(endRecord);
            fuzzyCheckpoint = decodeFuzzyCheckpointEndLogPayload(endRecord.payload);
            stats.checkpointActiveTransactionCount = fuzzyCheckpoint->activeTransactions.size();
            stats.checkpointMode = CheckpointMode::Fuzzy;
            stats.checkpointDirtyPageCount = fuzzyCheckpoint->dirtyPages.size();
            for (const auto& entry : fuzzyCheckpoint->dirtyPages) {
                dirtyPageTable.emplace(entry.pageId, entry.recLsn);
                if (!isValidLsn(stats.oldestCheckpointRecLsn)
                    || entry.recLsn < stats.oldestCheckpointRecLsn) {
                    stats.oldestCheckpointRecLsn = entry.recLsn;
                }
            }
        }
    }
    const auto analysisBoundary = stats.recoveryStartOffset;
    auto scanStart = analysisBoundary;
    if (fuzzyCheckpoint.has_value() && isValidLsn(stats.oldestCheckpointRecLsn)) {
        scanStart = std::min(scanStart, stats.oldestCheckpointRecLsn);
    }
    std::map<TransactionId, CheckpointTransactionEntry> checkpointAtt;
    if (fuzzyCheckpoint.has_value()) {
        for (const auto& entry : fuzzyCheckpoint->activeTransactions) {
            if (entry.beginLsn < logManager_.oldestRetainedLsn()
                || entry.lastLsn >= analysisBoundary
                || entry.transactionId >= fuzzyCheckpoint->nextTransactionId) {
                throw WalError(WalErrorKind::CorruptRecord, "Checkpoint ATT chain is unavailable or invalid");
            }
            scanStart = std::min(scanStart, entry.beginLsn);
            checkpointAtt.emplace(entry.transactionId, entry);
        }
    }
    stats.walBytesSkipped = scanStart - logManager_.oldestRetainedLsn();
    auto scan = logManager_.scanFrom(scanStart);
    stats.walBytesScanned = scan.fileBytes - scanStart;
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
    std::map<PageId, TransactionId> activePageOwners;
    std::map<CheckpointId, CheckpointBeginLogPayload> checkpointBegins;
    std::map<CheckpointId, Lsn> fuzzyCheckpointBegins;
    TransactionId highestTailTransactionId = 0;
    std::vector<const LogRecord*> redo;
    std::unordered_map<Lsn, const LogRecord*> recordsByLsn;
    recordsByLsn.reserve(scan.records.size());
    for (const auto& record : scan.records) recordsByLsn.emplace(record.lsn, &record);
    stats.lsnIndexEntries = recordsByLsn.size();
    stats.recoveryLsnIndexEntries = recordsByLsn.size();
    stats.recoveryLsnIndexBytes = recordsByLsn.size() * sizeof(decltype(recordsByLsn)::value_type)
        + recordsByLsn.bucket_count() * sizeof(void*);
    bool checkedAtt = false;
    const auto checkAtt = [&] {
        for (const auto& [id, entry] : checkpointAtt) {
            const auto found = transactions.find(id);
            if (found == transactions.end() || found->second.status != TransactionStatus::Active
                || found->second.beginLsn != entry.beginLsn
                || found->second.lastLsn != entry.lastLsn
                || found->second.startPageCount != entry.startPageCount) {
                throw WalError(WalErrorKind::CorruptRecord, "Checkpoint ATT disagrees with retained transaction chain");
            }
        }
    };
    for (const auto& record : scan.records) {
        highestTailTransactionId = std::max(highestTailTransactionId, record.transactionId);
        if (record.lsn >= analysisBoundary && !checkedAtt) {
            checkAtt();
            checkedAtt = true;
        }
        if (record.lsn < analysisBoundary && !checkpointAtt.contains(record.transactionId)) {
            if (isPageAffectingRecord(record.type)) {
                validateTransactionRecordPayload(record);
                redo.push_back(&record);
            } else if (record.type == LogRecordType::CheckpointBegin
                       || record.type == LogRecordType::CheckpointEnd
                       || record.type == LogRecordType::FuzzyCheckpointBegin
                       || record.type == LogRecordType::FuzzyCheckpointEnd) {
                validateCheckpointRecord(record);
            } else {
                validateTransactionRecordPayload(record);
            }
            continue;
        }
        ++stats.recordsAnalyzed;
        if (record.type == LogRecordType::CheckpointBegin
            || record.type == LogRecordType::CheckpointEnd
            || record.type == LogRecordType::FuzzyCheckpointBegin
            || record.type == LogRecordType::FuzzyCheckpointEnd) {
            validateCheckpointRecord(record);
            if ((record.type == LogRecordType::CheckpointBegin
                 || record.type == LogRecordType::CheckpointEnd)
                && std::any_of(transactions.begin(), transactions.end(), [](const auto& entry) {
                    return entry.second.status == TransactionStatus::Active
                        || entry.second.status == TransactionStatus::Aborting;
                })) {
                throw WalError(WalErrorKind::CorruptRecord,
                               "Checkpoint record overlaps an active transaction");
            }
            if (record.type == LogRecordType::CheckpointBegin) {
                const auto payload = decodeCheckpointBeginLogPayload(record.payload);
                checkpointBegins[payload.checkpointId] = payload;
                stats.highestCheckpointId = std::max(stats.highestCheckpointId,
                                                      payload.checkpointId);
            } else if (record.type == LogRecordType::CheckpointEnd) {
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
            } else if (record.type == LogRecordType::FuzzyCheckpointBegin) {
                const auto payload = decodeFuzzyCheckpointBeginLogPayload(record.payload);
                fuzzyCheckpointBegins[payload.checkpointId] = record.lsn;
                stats.highestCheckpointId = std::max(stats.highestCheckpointId,
                                                      payload.checkpointId);
            } else {
                const auto payload = decodeFuzzyCheckpointEndLogPayload(record.payload);
                const auto foundBegin = fuzzyCheckpointBegins.find(payload.checkpointId);
                if (foundBegin == fuzzyCheckpointBegins.end()
                    || payload.checkpointBeginLsn != foundBegin->second) {
                    throw WalError(WalErrorKind::CorruptRecord,
                                   "FUZZY_CHECKPOINT_END does not match a preceding BEGIN");
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
            if (found != transactions.end()) {
                throw WalError(WalErrorKind::CorruptRecord, "Transaction has duplicate BEGIN records");
            }
            AnalyzedTransaction transaction;
            transaction.transactionId = record.transactionId;
            transaction.startPageCount = decodeBeginLogPayload(record.payload).startPageCount;
            transaction.beginLsn = record.lsn;
            transaction.lastLsn = record.lsn;
            transaction.undoNextLsn = record.lsn;
            transactions.emplace(record.transactionId, std::move(transaction));
            continue;
        }
        if (found == transactions.end()
            || (found->second.status != TransactionStatus::Active
                && found->second.status != TransactionStatus::Aborting)
            || record.prevLsn >= record.lsn
            || record.prevLsn != found->second.lastLsn) {
            throw WalError(WalErrorKind::CorruptRecord, "WAL transaction chain is malformed");
        }
        if (isPageUpdateRecord(record.type)) {
            if (found->second.status != TransactionStatus::Active) {
                throw WalError(WalErrorKind::CorruptRecord, "Page update after transaction entered UNDO");
            }
            const auto identity = pageUpdateIdentity(record);
            const auto pageId = identity.pageId;
            const auto beforePageExisted = identity.beforePageExisted;
            if (beforePageExisted != (pageId < found->second.startPageCount)) {
                throw WalError(WalErrorKind::CorruptRecord,
                               "Page-update existence flag contradicts transaction BEGIN");
            }
            const auto [owner, inserted] = activePageOwners.emplace(pageId, record.transactionId);
            if (!inserted && owner->second != record.transactionId) {
                throw WalError(WalErrorKind::CorruptRecord, "Conflicting active physical page histories are unsupported");
            }
            found->second.updates.push_back(&record);
            if (fuzzyCheckpoint.has_value() && !dirtyPageTable.contains(pageId)) {
                dirtyPageTable.emplace(pageId, record.lsn);
            }
        } else if (record.type == LogRecordType::Compensation) {
            const auto compensation = decodeCompensationLogPayload(record.payload);
            if (compensation.pageId >= found->second.startPageCount) {
                throw WalError(
                    WalErrorKind::CorruptRecord,
                    "CLR cannot compensate a page created by the loser transaction");
            }
            const auto compensated = recordsByLsn.find(
                compensation.compensatedUpdateLsn);
            if (compensated == recordsByLsn.end()
                || compensated->second->transactionId != record.transactionId
                || !isPageUpdateRecord(compensated->second->type)
                || compensated->second->lsn >= record.lsn
                || compensated->second->prevLsn != compensation.undoNextLsn
                || pageUpdateIdentity(*compensated->second).pageId
                    != compensation.pageId) {
                throw WalError(
                    WalErrorKind::CorruptRecord,
                    "CLR does not identify a matching transaction update");
            }
            auto target = found->second.status == TransactionStatus::Aborting
                ? found->second.undoNextLsn : record.prevLsn;
            while (isValidLsn(target)) {
                const auto prior = recordsByLsn.find(target);
                if (prior == recordsByLsn.end() || prior->second->transactionId != record.transactionId
                    || prior->second->lsn >= record.lsn) {
                    throw WalError(WalErrorKind::CorruptRecord, "CLR undoNext chain is malformed");
                }
                if (!isPageUpdateRecord(prior->second->type)
                    || pageUpdateIdentity(*prior->second).beforePageExisted) break;
                target = prior->second->prevLsn;
            }
            if (target != compensation.compensatedUpdateLsn) {
                throw WalError(WalErrorKind::CorruptRecord, "CLR skips or repeats an uncompensated update");
            }
            found->second.status = TransactionStatus::Aborting;
            found->second.undoNextLsn = compensation.undoNextLsn;
            ++found->second.clrCount;
            found->second.clrs.push_back(&record);
            ++stats.analyzedClrCount;
            if (fuzzyCheckpoint.has_value()
                && !dirtyPageTable.contains(compensation.pageId)) {
                dirtyPageTable.emplace(compensation.pageId, record.lsn);
            }
        } else if (record.type == LogRecordType::Commit) {
            if (found->second.status != TransactionStatus::Active) {
                throw WalError(WalErrorKind::CorruptRecord, "COMMIT after rollback began");
            }
            found->second.status = TransactionStatus::Committed;
            found->second.hasDurableCommit = true;
            ++stats.committedTransactions;
        } else if (record.type == LogRecordType::Abort) {
            found->second.status = TransactionStatus::Aborted;
            found->second.hasDurableAbort = true;
            ++stats.abortedTransactions;
            stats.durableAbortObserved = true;
        }
        found->second.lastLsn = record.lsn;
        if (found->second.status == TransactionStatus::Active) found->second.undoNextLsn = record.lsn;
        if (found->second.hasDurableCommit || found->second.hasDurableAbort) {
            found->second.undoNextLsn = INVALID_LSN;
            for (const auto* update : found->second.updates) {
                activePageOwners.erase(pageUpdateIdentity(*update).pageId);
            }
        }
    }
    if (!checkedAtt) checkAtt();
    stats.transactionsAnalyzed = transactions.size();
    stats.analysisTransactions = transactions.size();
    stats.peakRecoveryTransactionTableSize = transactions.size();
    TransactionId baseNext = checkpoint.slot.has_value()
        ? checkpoint.slot->nextTransactionId : TransactionId{1};
    if (highestTailTransactionId == std::numeric_limits<TransactionId>::max()) {
        throw std::overflow_error("WAL transaction ID space is exhausted");
    }
    stats.nextTransactionId = std::max(baseNext, highestTailTransactionId + 1);

    std::vector<RecoveryTransactionEntry*> losers;
    for (auto& [id, transaction] : transactions) {
        static_cast<void>(id);
        if (transaction.status == TransactionStatus::Committed) {
            redo.insert(redo.end(), transaction.updates.begin(), transaction.updates.end());
        } else if (transaction.status == TransactionStatus::Active
                   || transaction.status == TransactionStatus::Aborting) {
            losers.push_back(&transaction);
            ++stats.loserTransactions;
            if (transaction.status == TransactionStatus::Active) ++stats.analysisActiveTransactions;
            else ++stats.analysisAbortingTransactions;
            if (!transaction.clrs.empty()) ++stats.undoRestartCount;
        }
        analysisTransactions_.push_back(transaction);
        redo.insert(redo.end(), transaction.clrs.begin(), transaction.clrs.end());
    }
    std::sort(redo.begin(), redo.end(), [](const LogRecord* left, const LogRecord* right) {
        return left->lsn < right->lsn;
    });
    if (!losers.empty()) {
        stats.loserLastLsn = losers.front()->lastLsn;
        stats.loserLastUndoNextLsn = losers.front()->undoNextLsn;
        // Allocation ownership is deliberately not redesigned here. Multiple
        // losers are supported only on preallocated, disjoint physical pages.
        const auto minimumStart = (*std::min_element(losers.begin(), losers.end(),
            [](const auto* left, const auto* right) { return left->startPageCount < right->startPageCount; }))
            ->startPageCount;
        for (const auto& [id, transaction] : transactions) {
            static_cast<void>(id);
            if (losers.size() > 1 && transaction.startPageCount != diskManager_.pageCount()) {
                throw WalError(WalErrorKind::CorruptRecord, "Interleaved allocation history is unsupported");
            }
            if (transaction.hasDurableCommit) {
                for (const auto* update : transaction.updates) {
                    if (pageUpdateIdentity(*update).pageId >= minimumStart) {
                        throw WalError(WalErrorKind::CorruptRecord, "Loser truncation would remove a winner page");
                    }
                }
            }
        }
    }
    stats.analysisWinners = stats.committedTransactions;
    stats.analysisLosers = stats.loserTransactions;
    stats.analysisAborted = stats.abortedTransactions;
    stats.redoCandidates = redo.size();
    if (fuzzyCheckpoint.has_value() && !dirtyPageTable.empty()) {
        stats.redoStartLsn = std::min_element(
            dirtyPageTable.begin(), dirtyPageTable.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; })
            ->second;
    }
    stats.analysisNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - analysisStart).count());
    const auto redoStart = std::chrono::steady_clock::now();
    for (const auto* record : redo) {
        if (fuzzyCheckpoint.has_value()) {
            const auto identity = pageUpdateIdentity(*record);
            const auto foundDpt = dirtyPageTable.find(identity.pageId);
            if (foundDpt == dirtyPageTable.end()) {
                ++stats.redoSkippedNotInDpt;
                continue;
            }
            if (record->lsn < foundDpt->second) {
                ++stats.redoSkippedBeforeRecLsn;
                continue;
            }
        }
        const auto beforeCount = diskManager_.pageCount();
        bool applied = true;
        if (record->type == LogRecordType::Compensation) {
            const auto compensation = decodeCompensationLogPayload(record->payload);
            if (compensation.pageId >= diskManager_.pageCount()) {
                throw WalError(WalErrorKind::CorruptRecord,
                               "CLR references a missing physical page");
            }
            DiskManager::Page current{};
            if (redoPolicy_ == RedoPolicy::PageLsnSelectiveRedo
                && compensation.pageSupportsLsn) {
                diskManager_.readPhysicalPage(compensation.pageId, current);
                ++stats.recoveryPageReads;
                ++stats.pageLsnChecks;
                ++stats.redoPageLsnChecks;
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
                        ++stats.redoClrSkippedByPageLsn;
                    }
                }
            }
            if (applied) {
                current = compensation.compensatedImage;
                const bool imageSupportsLsn = supportsPersistentPageLsn(current);
                if (imageSupportsLsn != compensation.pageSupportsLsn) {
                    throw WalError(
                        WalErrorKind::CorruptRecord,
                        "CLR PageLSN flag disagrees with its compensation image");
                }
                if (imageSupportsLsn) writePersistentPageLsn(current, record->lsn);
                diskManager_.writePhysicalPage(compensation.pageId, current);
            }
        } else if (record->type == LogRecordType::PageUpdate) {
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
                ++stats.redoPageLsnChecks;
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
                        ++stats.redoUserUpdateSkippedByPageLsn;
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
            ++stats.redoApplied;
            if (record->type == LogRecordType::Compensation) {
                ++stats.redoClrApplied;
            } else {
                ++stats.redoUserUpdateApplied;
            }
            ++stats.databaseWrites;
            ++stats.recoveryPageWrites;
            ++stats.pagesRedone;
            recoveryFailPoint("recovery_after_redo_page");
        }
    }
    stats.redoNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - redoStart).count());

    if (!losers.empty()) {
        undoTransactions(
            diskManager_, logManager_, losers,
            [&](Lsn lsn) -> LogRecord {
                const auto found = recordsByLsn.find(lsn);
                if (found == recordsByLsn.end()) {
                    throw WalError(WalErrorKind::CorruptRecord,
                                   "UNDO chain references unavailable WAL history");
                }
                return *found->second;
            },
            [&](PageId pageId, DiskManager::Page& page) {
                diskManager_.readPhysicalPage(pageId, page);
                ++stats.recoveryPageReads;
            }, stats);
    } else {
        diskManager_.sync();
        ++stats.databaseSyncCalls;
        recoveryFailPoint("recovery_after_database_sync");
    }
    stats.multiLoserUndoSteps = stats.undoQueuePops;
    stats.multiLoserClrsAppended = stats.clrsAppended;
    stats.transactionsCompletedDuringUndo = stats.abortedTransactions - stats.analysisAborted;
    diskManager_.reloadDatabaseHeader();
    for (const auto& [id, transaction] : transactions) {
        static_cast<void>(id);
        recoveredTransactions_.push_back(transaction);
    }
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
    std::lock_guard lock(contextsMutex_);
    return (active_ != nullptr) ? active_->transactionId : INVALID_TRANSACTION_ID;
}

bool RecoveryCoordinator::hasMaterializedWalBegin(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    return (context != nullptr) && isValidLsn(context->beginLsn);
}

Lsn RecoveryCoordinator::lastLsn(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    return (context != nullptr) ? context->lastLsn : INVALID_LSN;
}

std::size_t RecoveryCoordinator::touchedPageCount(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    return (context != nullptr) ? context->pages.size() : 0;
}

std::uint64_t RecoveryCoordinator::transactionWalBytes(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    return (context != nullptr) ? context->walBytes : 0;
}

std::uint64_t RecoveryCoordinator::originalBeforeImageBytes(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    if (!(context != nullptr)) return 0;
    return static_cast<std::uint64_t>(std::count_if(
        context->pages.begin(), context->pages.end(),
        [](const auto& page) { return page.second.beforeExisted; }))
        * database_format::PAGE_SIZE;
}

std::uint64_t RecoveryCoordinator::transactionRecoveryBytes(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    if (!(context != nullptr)) return 0;
    return sizeof(RecoveryTransactionContext)
        + context->pages.size() * sizeof(decltype(context->pages)::value_type);
}

std::uint64_t RecoveryCoordinator::peakTransactionRecoveryBytes(TransactionId id) const noexcept {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    const auto* context = id == INVALID_TRANSACTION_ID ? active_
        : found == contexts_.end() ? nullptr : &found->second;
    return (context != nullptr) ? context->peakRecoveryBytes : 0;
}

TransactionId RecoveryCoordinator::beginTransaction() {
    std::lock_guard lock(contextsMutex_);
    if (nextTransactionId_ == INVALID_TRANSACTION_ID
        || nextTransactionId_ == std::numeric_limits<TransactionId>::max()) {
        throw std::overflow_error("Transaction ID space is exhausted");
    }
    const auto id = nextTransactionId_++;
    RecoveryTransactionContext context;
    context.transactionId = id;
    context.startPageCount = diskManager_.pageCount();
    auto [position, inserted] = contexts_.emplace(id, std::move(context));
    static_cast<void>(inserted);
    position->second.peakRecoveryBytes = sizeof(RecoveryTransactionContext);
    contextCount_ = contexts_.size();
    activeSignal_ = true;
    ++stats_.transactionsBegun;
    return id;
}

void RecoveryCoordinator::bindTransaction(TransactionId id) {
    std::lock_guard lock(contextsMutex_);
    const auto found = contexts_.find(id);
    if (found == contexts_.end()) throw std::logic_error("Unknown recovery transaction");
    if (rollbackActive_) throw std::logic_error("Rollback is in progress");
    if (active_ != nullptr && !active_->safeBoundary && active_->transactionId != id) {
        throw std::logic_error("Cannot switch a physically mutating transaction");
    }
    active_ = &found->second;
}

void RecoveryCoordinator::beginMutation(TransactionId id) {
    bindTransaction(id);
    std::lock_guard lock(contextsMutex_);
    active_->safeBoundary = false;
}

void RecoveryCoordinator::prepareTransaction(TransactionId id) {
    bindTransaction(id);
    prepareStatement();
}

void RecoveryCoordinator::commitTransaction(TransactionId id) {
    bindTransaction(id);
    commitStatement();
}

void RecoveryCoordinator::rollbackTransaction(TransactionId id) {
    bindTransaction(id);
    rollbackStatement();
}

void RecoveryCoordinator::beginStatement() {
    if (active_ != nullptr) throw std::logic_error("A mutating statement is already active");
    bindTransaction(beginTransaction());
}

bool RecoveryCoordinator::checkpointSafe() const {
    std::lock_guard lock(contextsMutex_);
    return !rollbackActive_ && std::all_of(contexts_.begin(), contexts_.end(),
        [](const auto& entry) { return entry.second.safeBoundary; });
}

std::vector<CheckpointTransactionEntry> RecoveryCoordinator::checkpointTransactions() const {
    std::lock_guard lock(contextsMutex_);
    if (!checkpointSafe()) throw std::logic_error("Checkpoint requires a safe statement boundary");
    std::vector<CheckpointTransactionEntry> entries;
    for (const auto& [id, context] : contexts_) {
        if (!isValidLsn(context.beginLsn)) continue;
        entries.push_back({id, CheckpointTransactionStatus::Active, context.beginLsn,
                           context.lastLsn, context.startPageCount});
    }
    return entries;
}

std::vector<RecoveryTransactionEntry> RecoveryCoordinator::transactionSnapshot() const {
    std::lock_guard lock(contextsMutex_);
    std::vector<RecoveryTransactionEntry> entries;
    for (const auto& [id, context] : contexts_) {
        static_cast<void>(id);
        entries.push_back(context);
    }
    return entries;
}

void RecoveryCoordinator::notePageWriteIntent(
    PageId pageId,
    const DiskManager::Page& before) {
    std::lock_guard lock(contextsMutex_);
    if (!(active_ != nullptr)) {
        throw std::logic_error("Page mutation requires an active statement transaction");
    }
    if (pageId == INVALID_PAGE_ID) throw std::invalid_argument("Write intent has invalid PageId");
    for (const auto& [id, context] : contexts_) {
        if (id != active_->transactionId && context.pages.contains(pageId)) {
            throw std::logic_error("Simultaneous physical page ownership is unsupported");
        }
    }
    active_->safeBoundary = false;
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
    active_->peakRecoveryBytes = std::max(
        active_->peakRecoveryBytes, transactionRecoveryBytes());
    ++stats_.pagesFirstWritten;
}

void RecoveryCoordinator::ensureBeginLogged(RecoveryTransactionContext& context) {
    if (isValidLsn(context.beginLsn)) return;
    const auto payload = encodeBeginLogPayload(BeginLogPayload{context.startPageCount});
    context.beginLsn = logManager_.append(LogRecord{
        LogRecordType::Begin,
        context.transactionId,
        INVALID_LSN,
        payload,
        INVALID_LSN,
    });
    stats_.walTotalBytesGenerated += wal_record_layout::HEADER_SIZE + payload.size();
    context.walBytes += wal_record_layout::HEADER_SIZE + payload.size();
    context.lastLsn = context.beginLsn;
    context.undoNextLsn = context.beginLsn;
    recoveryFailPoint("after_begin_append");
}

Lsn RecoveryCoordinator::appendTransactionRecord(
    RecoveryTransactionContext& context,
    LogRecordType type,
    std::vector<std::byte> payload) {
    ensureBeginLogged(context);
    const auto recordBytes = wal_record_layout::HEADER_SIZE + payload.size();
    const auto lsn = logManager_.append(LogRecord{
        type, context.transactionId, context.lastLsn, std::move(payload), INVALID_LSN,
    });
    stats_.walTotalBytesGenerated += recordBytes;
    context.walBytes += recordBytes;
    context.lastLsn = lsn;
    context.undoNextLsn = lsn;
    return lsn;
}

Lsn RecoveryCoordinator::preparePageForWrite(
    PageId pageId,
    DiskManager::Page& after) {
    std::lock_guard lock(contextsMutex_);
    RecoveryTransactionContext* context = nullptr;
    for (auto& [id, candidate] : contexts_) {
        static_cast<void>(id);
        if (candidate.pages.contains(pageId)) { context = &candidate; break; }
    }
    if (context == nullptr) return INVALID_LSN;
    const auto found = context->pages.find(pageId);
    if (found == context->pages.end()) return INVALID_LSN;
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
    lsn = appendTransactionRecord(*context, recordType, std::move(payload));
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

void RecoveryCoordinator::prepareStatement() {
    std::vector<PageId> pages;
    TransactionId id;
    {
        std::lock_guard lock(contextsMutex_);
        if (active_ == nullptr) throw std::logic_error("No statement transaction is active");
        if (rollbackActive_) throw std::logic_error("Transaction rollback is in progress");
        id = active_->transactionId;
        for (const auto& [pageId, state] : active_->pages) {
            static_cast<void>(state);
            pages.push_back(pageId);
        }
    }
    requireNoPins();
    if (bufferPool_ != nullptr) {
        for (const auto pageId : pages) bufferPool_->prepareResidentPageForCommit(pageId);
    }
    std::lock_guard lock(contextsMutex_);
    contexts_.at(id).safeBoundary = true;
}

void RecoveryCoordinator::commitStatement() {
    prepareStatement();
    std::lock_guard lock(contextsMutex_);
    if (!isValidLsn(active_->beginLsn)) {
        ++stats_.zeroWriteTransactions;
        ++stats_.transactionsCommitted;
        contexts_.erase(active_->transactionId);
        active_ = nullptr;
        contextCount_ = contexts_.size();
        activeSignal_ = !contexts_.empty();
        return;
    }
    recoveryFailPoint("before_commit_append");
    const auto commitLsn = appendTransactionRecord(*active_, LogRecordType::Commit);
    recoveryFailPoint("after_commit_append");
    logManager_.flushUpTo(commitLsn);
    ++stats_.commitFsyncs;
    recoveryFailPoint("after_commit_sync");
    contexts_.erase(active_->transactionId);
    active_ = nullptr;
    contextCount_ = contexts_.size();
    activeSignal_ = !contexts_.empty();
    ++stats_.transactionsCommitted;
}

void RecoveryCoordinator::rollbackStatement() {
    if (!(active_ != nullptr)) throw std::logic_error("No statement transaction is active");
    // Log the final resident states before invalidation. In particular, the
    // transaction may have failed midway through its most recent SQL statement.
    prepareStatement();
    rollbackActive_ = true;
    lastRollbackStats_ = {};
    const auto startPageCount = active_->startPageCount;
    if (!hasMaterializedWalBegin() && diskManager_.pageCount() > startPageCount) {
        // Even an all-zero newly appended page must be removed on restart if
        // rollback is interrupted; BEGIN carries the transaction's boundary.
        std::lock_guard lock(contextsMutex_);
        ensureBeginLogged(*active_);
    }
    if (!hasMaterializedWalBegin()) {
        std::lock_guard lock(contextsMutex_);
        ++stats_.zeroWriteTransactions;
        ++stats_.transactionsRolledBack;
        contexts_.erase(active_->transactionId);
        active_ = nullptr;
        contextCount_ = contexts_.size();
        activeSignal_ = !contexts_.empty();
        rollbackActive_ = false;
        return;
    }
    logManager_.flushUpTo(active_->lastLsn);
    if (bufferPool_ != nullptr) {
        for (const auto& [pageId, state] : active_->pages) {
            static_cast<void>(state);
            bufferPool_->discardPageForRecovery(pageId);
        }
        bufferPool_->discardPagesAtOrAboveForRecovery(static_cast<PageId>(startPageCount));
    }
    std::lock_guard undoLock(contextsMutex_);
    lastRollbackStats_.loserTransactions = 1;
    undoTransactions(
        diskManager_, logManager_, {active_},
        [&](Lsn lsn) { return logManager_.readRecordAt(lsn); },
        [&](PageId pageId, DiskManager::Page& page) {
            const auto found = active_->pages.find(pageId);
            if (found == active_->pages.end() || !found->second.beforeExisted) {
                throw std::logic_error("Rollback page has no original before-image");
            }
            page = found->second.before;
        }, lastRollbackStats_, true);
    stats_.walTotalBytesGenerated += lastRollbackStats_.undoWalBytes
        + wal_record_layout::HEADER_SIZE;
    stats_.rollbackDatabaseWrites += lastRollbackStats_.undoPageWrites;
    diskManager_.reloadDatabaseHeader();
    std::lock_guard lock(contextsMutex_);
    contexts_.erase(active_->transactionId);
    active_ = nullptr;
    contextCount_ = contexts_.size();
    activeSignal_ = !contexts_.empty();
    rollbackActive_ = false;
    ++stats_.transactionsRolledBack;
}

} // namespace minidb
