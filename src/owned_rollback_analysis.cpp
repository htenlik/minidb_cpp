#include "minidb/owned_rollback_analysis.hpp"
#include "minidb/checkpoint_log.hpp"

#include <algorithm>

namespace minidb {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw WalError(WalErrorKind::CorruptRecord, message);
}
struct UpdateIdentity { PageId page; bool existed; };
bool isPageUpdateRecord(LogRecordType type) {
    return type == LogRecordType::PageUpdate || type == LogRecordType::PageDeltaUpdate
        || type == LogRecordType::PageUpdateV2 || type == LogRecordType::PageDeltaUpdateV2;
}
UpdateIdentity identity(const LogRecord& r) {
    switch (r.type) {
    case LogRecordType::PageUpdate: {
        const auto v = decodePageUpdateLogPayload(r.payload); return {v.pageId, v.beforePageExisted};
    }
    case LogRecordType::PageDeltaUpdate: {
        const auto v = decodePageDeltaUpdateLogPayload(r.payload); return {v.pageId, v.beforePageExisted};
    }
    case LogRecordType::PageUpdateV2: {
        const auto v = decodePageUpdateV2LogPayload(r.payload); return {v.pageId, v.beforePageExisted};
    }
    case LogRecordType::PageDeltaUpdateV2: {
        const auto v = decodePageDeltaUpdateV2LogPayload(r.payload); return {v.pageId, v.beforePageExisted};
    }
    default: throw WalError(WalErrorKind::CorruptRecord, "Expected a page update");
    }
}
bool terminal(OwnedCleanupPhase phase) {
    return phase == OwnedCleanupPhase::AbortRecorded || phase == OwnedCleanupPhase::CommitRecorded;
}
bool cleanupComplete(const OwnedRollbackAnalysis& t) {
    return std::all_of(t.reservations.begin(), t.reservations.end(), [](const auto& item) {
        return isValidLsn(item.second.progressLsn);
    });
}
}

std::map<TransactionId, OwnedRollbackAnalysis> analyzeOwnedRollbackHistory(std::span<const LogRecord> records) {
    std::map<TransactionId, OwnedRollbackAnalysis> transactions;
    std::map<Lsn, const LogRecord*> byLsn;
    std::map<TransactionId, Lsn> undoNext;
    std::map<PageId, TransactionId> pageOwners;
    std::optional<std::pair<TransactionId, CleanupIntentPayload>> pendingReclaim;
    Lsn end = records.empty() ? wal_file_layout::HEADER_SIZE : records.front().lsn;
    const auto claim = [&](PageId page, TransactionId owner) {
        const auto [it, inserted] = pageOwners.emplace(page, owner);
        require(inserted || it->second == owner, "Conflicting physical page ownership");
    };
    // Skip only work that never requires a page CLR. Existing-page work must
    // be consumed by an exact CLR, not waved away by a cleanup marker.
    const auto nextRequiredUndo = [&](Lsn target, TransactionId owner, Lsn begin) {
        while (target != begin) {
            const auto it = byLsn.find(target);
            require(it != byLsn.end() && it->second->transactionId == owner, "UNDO reference is not an owned record boundary");
            const auto& r = *it->second;
            if (r.type != LogRecordType::PageReserve && !(isPageUpdateRecord(r.type) && !identity(r).existed)) break;
            target = r.prevLsn;
        }
        return target;
    };
    for (const auto& r : records) {
        require(isValidLogRecordType(r.type) && isValidLsn(r.lsn) && r.lsn >= wal_file_layout::HEADER_SIZE
            && r.lsn == end && r.payload.size() <= wal_record_layout::MAX_PAYLOAD_SIZE
            && r.lsn < INVALID_LSN - wal_record_layout::HEADER_SIZE - r.payload.size(),
            "Noncontiguous/overflowed WAL record boundaries");
        end = r.lsn + wal_record_layout::HEADER_SIZE + r.payload.size();
        require(byLsn.emplace(r.lsn, &r).second, "Duplicate WAL record boundary");
        if (pendingReclaim) {
            const auto [owner, expected] = *pendingReclaim;
            require(r.lsn == expected.reclaimLsn && r.type == LogRecordType::PageReclaim,
                    "Cleanup intent's next record is not its associated reclaim");
            require(decodePageReclaim(r) == expected.reclaim, "Cleanup reclaim payload does not match intent");
            transactions.at(owner).reservations.at(expected.reclaim.page).reclaimLsn = r.lsn;
            pendingReclaim.reset();
            continue;
        }
        if (r.type == LogRecordType::PageReclaim) {
            static_cast<void>(decodePageReclaim(r));
            require(std::none_of(transactions.begin(), transactions.end(), [](const auto& t) {
                return !terminal(t.second.phase);
            }), "Unassociated system reclaim overlaps a transaction");
            continue;
        }
        if (r.type == LogRecordType::CheckpointBegin || r.type == LogRecordType::CheckpointEnd
            || r.type == LogRecordType::FuzzyCheckpointBegin || r.type == LogRecordType::FuzzyCheckpointEnd) {
            validateCheckpointRecord(r);
            // Stage 1 does not certify checkpoint/retention during cleanup.
            require(std::none_of(transactions.begin(), transactions.end(), [](const auto& item) {
                return isValidLsn(item.second.cleanupBeginLsn) && !terminal(item.second.phase);
            }), "Checkpoint during incomplete owned cleanup");
            continue;
        }
        validateTransactionRecordPayload(r);
        if (r.type == LogRecordType::Begin || r.type == LogRecordType::BeginOwned) {
            require(!transactions.contains(r.transactionId), "Duplicate/mixed transaction BEGIN");
            OwnedRollbackAnalysis t;
            t.transactionId = r.transactionId;
            t.protocol = r.type == LogRecordType::BeginOwned ? RollbackProtocol::OwnedAppend : RollbackProtocol::LegacyTruncate;
            t.startPageCount = t.protocol == RollbackProtocol::OwnedAppend
                ? decodeOwnedBegin(r).startPageCount : decodeBeginLogPayload(r.payload).startPageCount;
            t.beginLsn = t.lastLsn = r.lsn;
            transactions.emplace(r.transactionId, std::move(t));
            continue;
        }
        const auto found = transactions.find(r.transactionId);
        require(found != transactions.end(), "Transaction record without BEGIN");
        auto& t = found->second;
        require(!terminal(t.phase) && r.prevLsn == t.lastLsn && r.prevLsn < r.lsn,
                "Wrong owner, non-boundary prevLSN or record after termination");
        const bool owned = t.protocol == RollbackProtocol::OwnedAppend;
        require(!isOwnedRollbackRecord(r.type) || owned, "Owned cleanup in legacy transaction");
        const bool cleaning = isValidLsn(t.cleanupBeginLsn);
        require(!cleaning || r.type == LogRecordType::RollbackCleanupIntent
            || r.type == LogRecordType::RollbackCleanupProgress || r.type == LogRecordType::Abort,
            "Mutation/CLR/commit after logical UNDO completion");
        if (r.type == LogRecordType::PageReserve) {
            const auto v = decodePageReserveLogPayload(r.payload);
            require(t.phase == OwnedCleanupPhase::OwnedActive && v.expectedOldPageCount >= t.startPageCount
                && !t.reservations.contains(v.pageId)
                && (t.reservations.empty() || v.pageId > t.reservations.rbegin()->first), "Invalid/duplicate reservation identity");
            claim(v.pageId, r.transactionId);
            t.reservations.emplace(v.pageId, OwnedReservationEvidence{
                PageReservationEntry{v.pageId, r.lsn, v.source, v.expectedOldPageCount, false}});
        } else if (isPageUpdateRecord(r.type)) {
            const auto v = identity(r);
            require(t.phase == OwnedCleanupPhase::OwnedActive && v.existed == (v.page < t.startPageCount),
                    "Page update state/extent inconsistent with BEGIN");
            require(!owned || v.existed || t.reservations.contains(v.page), "Owned append update lacks reservation");
            claim(v.page, r.transactionId);
            (v.existed ? t.preexistingUpdates : t.appendedUpdates)[v.page] = r.lsn;
        } else if (r.type == LogRecordType::Compensation) {
            const auto v = decodeCompensationLogPayload(r.payload);
            const auto original = byLsn.find(v.compensatedUpdateLsn);
            require(original != byLsn.end() && original->second->transactionId == r.transactionId
                && isPageUpdateRecord(original->second->type) && identity(*original->second).existed
                && identity(*original->second).page == v.pageId && original->second->prevLsn == v.undoNextLsn,
                "CLR does not compensate its owner's existing-page update");
            const auto target = nextRequiredUndo(t.phase == OwnedCleanupPhase::Undoing ? undoNext.at(r.transactionId) : r.prevLsn,
                                                 r.transactionId, t.beginLsn);
            require(target == v.compensatedUpdateLsn, "CLR skips/repeats required logical UNDO");
            undoNext[r.transactionId] = v.undoNextLsn;
            t.phase = OwnedCleanupPhase::Undoing;
        } else if (r.type == LogRecordType::RollbackCleanupBegin) {
            const auto v = decodeCleanupBegin(r);
            require(v.undoEnd == t.beginLsn && v.reservationCount == t.reservations.size(), "Cleanup BEGIN lineage/count mismatch");
            const auto target = t.phase == OwnedCleanupPhase::Undoing ? undoNext.at(r.transactionId) : r.prevLsn;
            require(nextRequiredUndo(target, r.transactionId, t.beginLsn) == t.beginLsn,
                    "Cleanup before required logical UNDO completion");
            t.logicalUndoChainComplete = true;
            t.phase = OwnedCleanupPhase::LogicalUndoComplete;
            t.cleanupBeginLsn = r.lsn;
            t.phase = cleanupComplete(t) ? OwnedCleanupPhase::CleanupComplete : OwnedCleanupPhase::CleanupStarted;
        } else if (r.type == LogRecordType::RollbackCleanupIntent) {
            const auto v = decodeCleanupIntent(r);
            const auto reservation = t.reservations.find(v.reclaim.page);
            require(cleaning && reservation != t.reservations.end()
                && reservation->second.reservation.reserveLsn == v.reserveLsn
                && !isValidLsn(reservation->second.intentLsn) && !isValidLsn(reservation->second.progressLsn),
                "Intent lacks a unique owned reservation/cleanup boundary");
            require(std::none_of(transactions.begin(), transactions.end(), [&](const auto& item) {
                return item.first != r.transactionId && !terminal(item.second.phase);
            }), "Cleanup cannot overlap another live transaction");
            reservation->second.intentLsn = r.lsn;
            t.phase = OwnedCleanupPhase::ReservationCleanupInProgress;
            pendingReclaim = std::pair{r.transactionId, v};
        } else if (r.type == LogRecordType::RollbackCleanupProgress) {
            const auto v = decodeCleanupProgress(r);
            const auto reservation = t.reservations.find(v.page);
            require(cleaning && reservation != t.reservations.end()
                && reservation->second.reservation.reserveLsn == v.reserveLsn
                && !isValidLsn(reservation->second.progressLsn), "Progress lacks owned reservation or is repeated");
            auto& entry = reservation->second;
            if (isValidLsn(v.reclaimLsn)) {
                require(isValidLsn(entry.intentLsn) && entry.reclaimLsn == v.reclaimLsn
                    && decodeCleanupIntent(*byLsn.at(entry.intentLsn)).reclaim.pageCount == v.observedExtent,
                    "Progress without matching intent/reclaim/extent");
                entry.outcome = ReservationCleanupOutcome::ReclaimRecorded;
            } else {
                // Prototype's explicit absent-page completion, not a reclaim.
                // Physical absence/lease/fsync must be established by stage 2.
                require(!isValidLsn(entry.intentLsn) && v.observedExtent >= t.startPageCount,
                        "Absent progress conflicts with reclaim intent/BEGIN extent");
                entry.outcome = ReservationCleanupOutcome::AbsenceRecorded;
            }
            entry.progressLsn = r.lsn;
            t.phase = cleanupComplete(t) ? OwnedCleanupPhase::CleanupComplete : OwnedCleanupPhase::ReservationCleanupInProgress;
        } else if (r.type == LogRecordType::Commit) {
            require(t.phase == OwnedCleanupPhase::OwnedActive, "COMMIT after rollback started");
            t.phase = OwnedCleanupPhase::CommitRecorded;
        } else if (r.type == LogRecordType::Abort) {
            require(!owned || t.phase == OwnedCleanupPhase::CleanupComplete, "Owned ABORT before cleanup complete");
            t.phase = OwnedCleanupPhase::AbortRecorded;
        } else {
            require(false, "Unsupported transaction record");
        }
        t.lastLsn = r.lsn;
        if (terminal(t.phase)) {
            std::erase_if(pageOwners, [&](const auto& item) { return item.second == r.transactionId; });
        }
    }
    // An intent-only tail is valid incomplete evidence, never auto-completed.
    return transactions;
}
} // namespace minidb
