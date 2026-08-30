#include "minidb/checkpoint_manager.hpp"

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/disk_manager.hpp"
#include "minidb/log_manager.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace minidb {

CheckpointManager::CheckpointManager(
    RecoveryCoordinator& recovery,
    BufferPoolManager& bufferPool,
    DiskManager& diskManager,
    LogManager& logManager,
    CheckpointControl& control,
    const RecoveryStats& startupRecovery,
    CheckpointPolicy policy)
    : recovery_(recovery), bufferPool_(bufferPool), diskManager_(diskManager),
      logManager_(logManager), control_(control), policy_(policy),
      lastGeneration_(startupRecovery.checkpointGeneration),
      previousCheckpointEndLsn_(startupRecovery.checkpointEndLsn),
      lastCheckpointWalSize_(startupRecovery.checkpointUsed
          ? startupRecovery.checkpointWalHighWater : wal_file_layout::HEADER_SIZE) {
    const auto highest = startupRecovery.highestCheckpointId;
    if (highest == std::numeric_limits<CheckpointId>::max()) {
        throw std::overflow_error("Checkpoint ID space is exhausted");
    }
    nextCheckpointId_ = highest == INVALID_CHECKPOINT_ID ? 1 : highest + 1;
}

CheckpointId CheckpointManager::checkpoint() {
    return checkpoint(policy_.mode);
}

CheckpointId CheckpointManager::checkpoint(CheckpointMode mode) {
    const auto started = std::chrono::steady_clock::now();
    ++stats_.checkpointsStarted;
    try {
        if (recovery_.hasActiveStatement() || recovery_.rollbackActive()) {
            throw std::logic_error("Checkpoint requires no active transaction or rollback");
        }
        if (mode == CheckpointMode::Sharp && bufferPool_.totalPinCount() != 0) {
            throw std::logic_error("Checkpoint requires zero pinned buffer frames");
        }
        if (nextCheckpointId_ == INVALID_CHECKPOINT_ID
            || lastGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Checkpoint identity space is exhausted");
        }

        const auto checkpointId = nextCheckpointId_;
        if (nextCheckpointId_ == std::numeric_limits<CheckpointId>::max()) {
            throw std::overflow_error("Checkpoint ID space is exhausted");
        }
        ++nextCheckpointId_;
        const auto walStart = logManager_.lastValidOffset();
        if (mode == CheckpointMode::Fuzzy) {
            const auto beginLsn = logManager_.append(LogRecord{
                LogRecordType::FuzzyCheckpointBegin,
                INVALID_TRANSACTION_ID,
                INVALID_LSN,
                encodeFuzzyCheckpointBeginLogPayload({
                    checkpointId, previousCheckpointEndLsn_,
                }),
                INVALID_LSN,
            });
            recoveryFailPoint("fuzzy_checkpoint_after_begin_append");
            const auto dirtyPages = bufferPool_.dirtyPageTableSnapshot();
            recoveryFailPoint("fuzzy_checkpoint_after_dpt_snapshot");
            const std::vector<CheckpointTransactionEntry> activeTransactions;
            const auto endPayload = encodeFuzzyCheckpointEndLogPayload({
                checkpointId,
                beginLsn,
                diskManager_.pageCount(),
                recovery_.nextTransactionId(),
                dirtyPages,
                activeTransactions,
            });
            const auto endLsn = logManager_.append(LogRecord{
                LogRecordType::FuzzyCheckpointEnd,
                INVALID_TRANSACTION_ID,
                INVALID_LSN,
                endPayload,
                INVALID_LSN,
            });
            const auto checkpointWalEnd = logManager_.lastValidOffset();
            recoveryFailPoint("fuzzy_checkpoint_after_end_append");
            logManager_.flushUpTo(endLsn);
            recoveryFailPoint("fuzzy_checkpoint_after_end_fsync");

            const auto controlBefore = control_.stats();
            control_.publish(CheckpointSlot{
                lastGeneration_ + 1,
                checkpointId,
                endLsn,
                beginLsn,
                diskManager_.pageCount(),
                recovery_.nextTransactionId(),
                checkpointWalEnd,
            });
            const auto controlAfter = control_.stats();
            recoveryFailPoint("fuzzy_checkpoint_after_control_sync");

            Lsn retentionFloor = beginLsn;
            for (const auto& entry : dirtyPages) {
                retentionFloor = std::min(retentionFloor, entry.recLsn);
            }
            const auto checkpointElapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
            const auto reclamationStarted = std::chrono::steady_clock::now();
            const auto segmentStatsBefore = logManager_.stats();
            std::uint64_t reclaimedBytes = 0;
            try {
                logManager_.rotateSegment();
                recoveryFailPoint("fuzzy_checkpoint_during_reclamation");
                reclaimedBytes = logManager_.reclaimSegmentsBefore(retentionFloor, 1);
            } catch (const std::exception&) {
                ++stats_.reclamationFailures;
            }
            const auto segmentStatsAfter = logManager_.stats();
            const auto reclamationElapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - reclamationStarted).count());

            ++stats_.checkpointsCompleted;
            ++stats_.fuzzyCheckpointsCompleted;
            ++stats_.walForces;
            stats_.controlFileSyncs += controlAfter.fsyncCalls - controlBefore.fsyncCalls;
            stats_.segmentsReclaimed += segmentStatsAfter.segmentsDeleted
                - segmentStatsBefore.segmentsDeleted;
            stats_.walBytesReclaimed += reclaimedBytes;
            stats_.reclamationDurationNs += reclamationElapsed;
            stats_.walBytesSincePreviousCheckpoint = walStart - lastCheckpointWalSize_;
            stats_.checkpointDurationNs += checkpointElapsed;
            stats_.checkpointMaxDurationNs = std::max(
                stats_.checkpointMaxDurationNs, checkpointElapsed);
            stats_.dptEntriesCaptured += dirtyPages.size();
            stats_.activeTransactionsCaptured += activeTransactions.size();
            stats_.pinnedFramesObserved += bufferPool_.stats().pinnedFrames;
            stats_.oldestRecLsn = dirtyPages.empty() ? INVALID_LSN : retentionFloor;
            stats_.retentionFloorLsn = retentionFloor;
            stats_.lastCheckpointId = checkpointId;
            stats_.lastCheckpointEndLsn = endLsn;
            stats_.lastRecoveryStartOffset = beginLsn;

            ++lastGeneration_;
            previousCheckpointEndLsn_ = endLsn;
            lastCheckpointWalSize_ = checkpointWalEnd;
            statementsSinceCheckpoint_ = 0;
            return checkpointId;
        }
        const auto beginLsn = logManager_.append(LogRecord{
            LogRecordType::CheckpointBegin,
            INVALID_TRANSACTION_ID,
            INVALID_LSN,
            encodeCheckpointBeginLogPayload({
                checkpointId, previousCheckpointEndLsn_, walStart,
            }),
            INVALID_LSN,
        });
        recoveryFailPoint("checkpoint_after_begin_append");

        const auto bufferBefore = bufferPool_.stats();
        bufferPool_.flushAll();
        const auto bufferAfter = bufferPool_.stats();
        const auto writes = bufferAfter.physicalPageWrites - bufferBefore.physicalPageWrites;
        recoveryFailPoint("checkpoint_after_buffer_flush");
        diskManager_.sync();
        recoveryFailPoint("checkpoint_after_database_sync");

        const auto endLsn = logManager_.lastValidOffset();
        constexpr std::uint64_t END_RECORD_SIZE = wal_record_layout::HEADER_SIZE
            + checkpoint_end_log_layout::PAYLOAD_SIZE;
        if (endLsn > std::numeric_limits<WalOffset>::max() - END_RECORD_SIZE) {
            throw std::overflow_error("WAL offset space is exhausted");
        }
        const auto recoveryStart = endLsn + END_RECORD_SIZE;
        const auto appendedEnd = logManager_.append(LogRecord{
            LogRecordType::CheckpointEnd,
            INVALID_TRANSACTION_ID,
            INVALID_LSN,
            encodeCheckpointEndLogPayload({
                checkpointId,
                beginLsn,
                diskManager_.pageCount(),
                recovery_.nextTransactionId(),
                recoveryStart,
            }),
            INVALID_LSN,
        });
        if (appendedEnd != endLsn || logManager_.lastValidOffset() != recoveryStart) {
            throw std::logic_error("CHECKPOINT_END size prediction disagrees with WAL encoding");
        }
        recoveryFailPoint("checkpoint_after_end_append");
        logManager_.flushUpTo(endLsn);
        recoveryFailPoint("checkpoint_after_end_fsync");

        const auto controlBefore = control_.stats();
        control_.publish(CheckpointSlot{
            lastGeneration_ + 1,
            checkpointId,
            endLsn,
            recoveryStart,
            diskManager_.pageCount(),
            recovery_.nextTransactionId(),
            recoveryStart,
        });
        const auto controlAfter = control_.stats();
        recoveryFailPoint("checkpoint_after_control_sync");

        const auto checkpointElapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        const auto reclamationStarted = std::chrono::steady_clock::now();
        const auto segmentStatsBefore = logManager_.stats();
        std::uint64_t reclaimedBytes = 0;
        try {
            logManager_.rotateSegment();
            // Keep one additional closed predecessor as a conservative local
            // recovery/debug cushion. It is still bounded and lets a torn newest
            // control slot fall back to the prior durable generation.
            reclaimedBytes = logManager_.reclaimSegmentsBefore(beginLsn, 1);
        } catch (const std::exception&) {
            // The sharp checkpoint is already durable and authoritative. Failed
            // best-effort reclamation only leaves extra history on disk.
            ++stats_.reclamationFailures;
        }
        const auto segmentStatsAfter = logManager_.stats();
        const auto reclamationElapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - reclamationStarted).count());

        ++stats_.checkpointsCompleted;
        ++stats_.sharpCheckpointsCompleted;
        stats_.dirtyPagesFlushed += writes;
        stats_.databaseWrites += writes;
        ++stats_.walForces;
        ++stats_.databaseSyncs;
        stats_.controlFileSyncs += controlAfter.fsyncCalls - controlBefore.fsyncCalls;
        stats_.segmentsReclaimed += segmentStatsAfter.segmentsDeleted
            - segmentStatsBefore.segmentsDeleted;
        stats_.walBytesReclaimed += reclaimedBytes;
        stats_.reclamationDurationNs += reclamationElapsed;
        stats_.walBytesSincePreviousCheckpoint = walStart - lastCheckpointWalSize_;
        stats_.checkpointDurationNs += checkpointElapsed;
        stats_.checkpointMaxDurationNs = std::max(
            stats_.checkpointMaxDurationNs, checkpointElapsed);
        stats_.lastCheckpointId = checkpointId;
        stats_.lastCheckpointEndLsn = endLsn;
        stats_.lastRecoveryStartOffset = recoveryStart;
        stats_.oldestRecLsn = INVALID_LSN;
        stats_.retentionFloorLsn = beginLsn;

        if (!bufferPool_.dirtyPageTableSnapshot().empty()) {
            throw std::logic_error("Successful sharp checkpoint left a nonempty DPT");
        }

        ++lastGeneration_;
        previousCheckpointEndLsn_ = endLsn;
        lastCheckpointWalSize_ = recoveryStart;
        statementsSinceCheckpoint_ = 0;
        return checkpointId;
    } catch (...) {
        ++stats_.checkpointFailures;
        throw;
    }
}

bool CheckpointManager::onStatementCommitted() noexcept {
    ++statementsSinceCheckpoint_;
    const auto walGrowth = logManager_.lastValidOffset() - lastCheckpointWalSize_;
    const bool walTriggered = policy_.walBytes != 0 && walGrowth >= policy_.walBytes;
    const bool statementTriggered = policy_.statements != 0
        && statementsSinceCheckpoint_ >= policy_.statements;
    if (!walTriggered && !statementTriggered) return false;
    try {
        static_cast<void>(checkpoint());
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace minidb
