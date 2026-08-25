#pragma once

#include "minidb/checkpoint_control.hpp"
#include "minidb/recovery.hpp"

#include <cstdint>

namespace minidb {

class BufferPoolManager;
class DiskManager;
class LogManager;

struct CheckpointPolicy {
    std::uint64_t walBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t statements = 0;
    bool operator==(const CheckpointPolicy&) const = default;
};

struct CheckpointStats {
    std::uint64_t checkpointsStarted = 0;
    std::uint64_t checkpointsCompleted = 0;
    std::uint64_t checkpointFailures = 0;
    std::uint64_t dirtyPagesFlushed = 0;
    std::uint64_t databaseWrites = 0;
    std::uint64_t walForces = 0;
    std::uint64_t databaseSyncs = 0;
    std::uint64_t controlFileSyncs = 0;
    std::uint64_t walBytesSincePreviousCheckpoint = 0;
    std::uint64_t checkpointDurationNs = 0;
    std::uint64_t checkpointMaxDurationNs = 0;
    CheckpointId lastCheckpointId = INVALID_CHECKPOINT_ID;
    Lsn lastCheckpointEndLsn = INVALID_LSN;
    WalOffset lastRecoveryStartOffset = wal_file_layout::HEADER_SIZE;
    bool operator==(const CheckpointStats&) const = default;
};

class CheckpointManager {
public:
    CheckpointManager(
        RecoveryCoordinator& recovery,
        BufferPoolManager& bufferPool,
        DiskManager& diskManager,
        LogManager& logManager,
        CheckpointControl& control,
        const RecoveryStats& startupRecovery,
        CheckpointPolicy policy = {});

    [[nodiscard]] CheckpointId checkpoint();
    [[nodiscard]] bool onStatementCommitted() noexcept;

    [[nodiscard]] const CheckpointPolicy& policy() const noexcept { return policy_; }
    [[nodiscard]] const CheckpointStats& stats() const noexcept { return stats_; }
    [[nodiscard]] CheckpointId nextCheckpointId() const noexcept { return nextCheckpointId_; }
    void resetStats() noexcept { stats_ = {}; }

private:
    RecoveryCoordinator& recovery_;
    BufferPoolManager& bufferPool_;
    DiskManager& diskManager_;
    LogManager& logManager_;
    CheckpointControl& control_;
    CheckpointPolicy policy_;
    CheckpointStats stats_{};
    CheckpointId nextCheckpointId_ = 1;
    std::uint64_t lastGeneration_ = 0;
    Lsn previousCheckpointEndLsn_ = INVALID_LSN;
    WalOffset lastCheckpointWalSize_ = wal_file_layout::HEADER_SIZE;
    std::uint64_t statementsSinceCheckpoint_ = 0;
};

} // namespace minidb
