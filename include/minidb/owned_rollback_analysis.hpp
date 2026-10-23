#pragma once

#include "minidb/owned_rollback_log.hpp"

namespace minidb {

enum class OwnedCleanupPhase : std::uint8_t {
    OwnedActive, Undoing, LogicalUndoComplete, CleanupStarted,
    ReservationCleanupInProgress, CleanupComplete, AbortRecorded, CommitRecorded,
};
enum class ReservationCleanupOutcome : std::uint8_t { Pending, ReclaimRecorded, AbsenceRecorded };

struct OwnedReservationEvidence {
    PageReservationEntry reservation;
    Lsn intentLsn = INVALID_LSN, reclaimLsn = INVALID_LSN, progressLsn = INVALID_LSN;
    ReservationCleanupOutcome outcome = ReservationCleanupOutcome::Pending;
};
struct OwnedRollbackAnalysis {
    TransactionId transactionId = 0;
    RollbackProtocol protocol = RollbackProtocol::LegacyTruncate;
    OwnedCleanupPhase phase = OwnedCleanupPhase::OwnedActive;
    Lsn beginLsn = INVALID_LSN, lastLsn = INVALID_LSN, cleanupBeginLsn = INVALID_LSN;
    std::uint64_t startPageCount = 0;
    bool logicalUndoChainComplete = false;
    std::map<PageId, OwnedReservationEvidence> reservations;
    // Preexisting updates (including free-list reuse/release) remain ordinary
    // transactional before-image/CLR work; no reuse provenance is invented.
    std::map<PageId, Lsn> preexistingUpdates;
    std::map<PageId, Lsn> appendedUpdates;
};

// Analysis ONLY, over a complete BEGIN-rooted, contiguous scanner result.
// No disk writes, tail repair, certificates, lease acquisition or authorization.
// A recorded progress/ABORT is a WAL claim, not proof of physical fsync. Runtime
// materialization is never inferred from PAGE_RESERVE after restart.
[[nodiscard]] std::map<TransactionId, OwnedRollbackAnalysis> analyzeOwnedRollbackHistory(
    std::span<const LogRecord> records);

} // namespace minidb
