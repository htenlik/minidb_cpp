#pragma once

#include "minidb/owned_rollback_analysis.hpp"

namespace minidb {

// Not an activation API or certificate issuer. Only the two recovery entry
// points can invoke this executor, under their private lifecycle lease binding.
// Ordinary PageReclaimer authorization and production admission are untouched.
class OwnedRollbackCleanup {
    friend class RecoveryManager;
    friend class RecoveryCoordinator;
    static void requireAuthority(const DatabaseAccessGate* gate, const DatabaseAccessGate::Lease* lease);
    static void completeIntentTail(DiskManager& disk, LogManager& log, WalScanResult& scan,
        Lsn begin, const DatabaseAccessGate* gate, const DatabaseAccessGate::Lease* lease);
    static void finish(DiskManager& disk, LogManager& log, RecoveryTransactionEntry& transaction,
        const DatabaseAccessGate* gate, const DatabaseAccessGate::Lease* lease, RecoveryStats& stats);
    static void validateReclaimEvidence(DiskManager& disk, const LogRecord& reclaim,
        const OwnedRollbackAnalysis& owner, std::span<const LogRecord> history);
};
} // namespace minidb
