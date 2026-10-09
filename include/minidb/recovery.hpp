#pragma once

#include "minidb/page_recovery.hpp"
#include <atomic>
#include "minidb/checkpoint_types.hpp"
#include "minidb/recovery_log.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace minidb {

class BufferPoolManager;
class CheckpointControl;
class LogManager;

enum class RedoPolicy : std::uint8_t {
    AlwaysRedo,
    PageLsnSelectiveRedo,
};

enum class RecoveryTransactionStatus : std::uint8_t { Active, Committed, Aborting, Aborted };

// Value-only snapshots: neither sessions nor page/frame pointers belong in ATT.
struct RecoveryTransactionEntry {
    TransactionId transactionId = INVALID_TRANSACTION_ID;
    RecoveryTransactionStatus status = RecoveryTransactionStatus::Active;
    Lsn beginLsn = INVALID_LSN;
    Lsn lastLsn = INVALID_LSN;
    Lsn undoNextLsn = INVALID_LSN;
    std::uint64_t startPageCount = 0;
    std::uint64_t clrCount = 0;
    bool hasDurableCommit = false;
    bool hasDurableAbort = false;
};

struct RecoveryStats {
    std::uint64_t analysisTransactions = 0;
    std::uint64_t analysisWinners = 0;
    std::uint64_t analysisLosers = 0;
    std::uint64_t analysisAborted = 0;
    std::uint64_t peakRecoveryTransactionTableSize = 0;
    std::uint64_t multiLoserUndoSteps = 0;
    std::uint64_t multiLoserClrsAppended = 0;
    std::uint64_t transactionsCompletedDuringUndo = 0;
    std::uint64_t recoveryLsnIndexEntries = 0;
    // Accounted index keys/pointers + bucket array; excludes allocator overhead
    // and scan-owned decoded payloads. O(retained scanned records).
    std::uint64_t recoveryLsnIndexBytes = 0;
    std::uint64_t checkpointActiveTransactionCount = 0;
    std::uint64_t analysisActiveTransactions = 0;
    std::uint64_t analysisAbortingTransactions = 0;
    std::uint64_t lsnIndexEntries = 0;
    std::uint64_t undoQueuePops = 0;
    std::uint64_t undoQueuePeak = 0;
    std::uint64_t recordsAnalyzed = 0;
    std::uint64_t transactionsAnalyzed = 0;
    std::uint64_t committedTransactions = 0;
    std::uint64_t abortedTransactions = 0;
    std::uint64_t loserTransactions = 0;
    std::uint64_t pagesRedone = 0;
    std::uint64_t pagesUndone = 0;
    std::uint64_t pagesTruncated = 0;
    std::uint64_t databasePagesExtended = 0;
    std::uint64_t databaseWrites = 0;
    std::uint64_t databaseSyncCalls = 0;
    std::uint64_t pageLsnChecks = 0;
    std::uint64_t redoPageLsnChecks = 0;
    std::uint64_t pageLsnUnknown = 0;
    std::uint64_t redoSkippedByPageLsn = 0;
    std::uint64_t redoAppliedAfterPageLsnCheck = 0;
    std::uint64_t redoCandidates = 0;
    std::uint64_t redoSkippedNotInDpt = 0;
    std::uint64_t redoSkippedBeforeRecLsn = 0;
    std::uint64_t redoApplied = 0;
    std::uint64_t redoUserUpdateApplied = 0;
    std::uint64_t redoUserUpdateSkippedByPageLsn = 0;
    std::uint64_t redoClrApplied = 0;
    std::uint64_t redoClrSkippedByPageLsn = 0;
    std::uint64_t legacyRedoRecords = 0;
    std::uint64_t analyzedClrCount = 0;
    std::uint64_t undoUserRecordsVisited = 0;
    std::uint64_t undoUserRecordsCompensated = 0;
    std::uint64_t undoClrsEncountered = 0;
    std::uint64_t clrsAppended = 0;
    std::uint64_t undoRecordsSkippedByClr = 0;
    std::uint64_t undoRestartCount = 0;
    std::uint64_t undoPageWrites = 0;
    std::uint64_t undoWalBytes = 0;
    std::uint64_t recoveryPageReads = 0;
    std::uint64_t recoveryPageWrites = 0;
    std::uint64_t tailBytesTruncated = 0;
    std::uint64_t analysisNs = 0;
    std::uint64_t redoNs = 0;
    std::uint64_t undoNs = 0;
    std::uint64_t totalNs = 0;
    bool checkpointControlPresent = false;
    bool checkpointUsed = false;
    CheckpointMode checkpointMode = CheckpointMode::Sharp;
    std::uint64_t checkpointDirtyPageCount = 0;
    Lsn oldestCheckpointRecLsn = INVALID_LSN;
    Lsn redoStartLsn = INVALID_LSN;
    CheckpointId checkpointId = INVALID_CHECKPOINT_ID;
    Lsn checkpointEndLsn = INVALID_LSN;
    WalOffset checkpointWalHighWater = wal_file_layout::HEADER_SIZE;
    WalOffset recoveryStartOffset = wal_file_layout::HEADER_SIZE;
    std::uint64_t walBytesSkipped = 0;
    std::uint64_t walBytesScanned = 0;
    bool fullScanFallback = true;
    std::uint64_t checkpointValidationFailures = 0;
    std::uint64_t checkpointGeneration = 0;
    CheckpointId highestCheckpointId = INVALID_CHECKPOINT_ID;
    TransactionId nextTransactionId = 1;
    Lsn loserLastLsn = INVALID_LSN;
    Lsn loserLastUndoNextLsn = INVALID_LSN;
    bool durableAbortObserved = false;
    bool repairedTail = false;

    [[nodiscard]] double redoSkipRatio() const noexcept {
        return pageLsnChecks == 0 ? 0.0
            : static_cast<double>(redoSkippedByPageLsn)
                / static_cast<double>(pageLsnChecks);
    }
};

struct TransactionRuntimeStats {
    std::uint64_t transactionsBegun = 0;
    std::uint64_t transactionsCommitted = 0;
    std::uint64_t transactionsRolledBack = 0;
    std::uint64_t zeroWriteTransactions = 0;
    std::uint64_t pagesFirstWritten = 0;
    std::uint64_t pageUpdateRecords = 0;
    std::uint64_t fullPageImageBytes = 0;
    std::uint64_t logicalBytesChanged = 0;
    std::uint64_t walUpdatePayloadBytes = 0;
    std::uint64_t walTotalBytesGenerated = 0;
    std::uint64_t rangeCount = 0;
    std::uint64_t changedBytes = 0;
    std::uint64_t updateRecordCount = 0;
    std::uint64_t fullPageUpdateRecords = 0;
    std::uint64_t byteRangeUpdateRecords = 0;
    std::uint64_t deltaComputationNs = 0;
    std::uint64_t adaptiveSelectionNs = 0;
    std::uint64_t adaptiveFullPageSelections = 0;
    std::uint64_t adaptiveDeltaSelections = 0;
    std::uint64_t adaptiveTies = 0;
    std::uint64_t bytesIfFullPage = 0;
    std::uint64_t bytesIfDelta = 0;
    std::uint64_t bytesActuallyChosen = 0;
    std::uint64_t bytesSavedByAdaptive = 0;
    std::uint64_t bytesSavedVersusByteRange = 0;
    std::uint64_t persistentPageLsnAssignments = 0;
    std::uint64_t v1PagesObserved = 0;
    std::uint64_t v2PagesWithKnownLsn = 0;
    std::uint64_t commitFsyncs = 0;
    std::uint64_t rollbackDatabaseWrites = 0;
    std::vector<std::uint64_t> updateRecordBytes;
    std::vector<std::uint64_t> deltaRangeCounts;
};

class RecoveryManager {
public:
    RecoveryManager(
        DiskManager& diskManager,
        LogManager& logManager,
        CheckpointControl* checkpointControl = nullptr,
        bool forceFullScan = false,
        RedoPolicy redoPolicy = RedoPolicy::PageLsnSelectiveRedo)
        : diskManager_(diskManager), logManager_(logManager),
          checkpointControl_(checkpointControl), forceFullScan_(forceFullScan),
          redoPolicy_(redoPolicy) {}

    [[nodiscard]] RecoveryStats recover();
    [[nodiscard]] const std::vector<RecoveryTransactionEntry>& analysisTransactions() const noexcept {
        return analysisTransactions_;
    }
    [[nodiscard]] const std::vector<RecoveryTransactionEntry>& recoveredTransactions() const noexcept {
        return recoveredTransactions_;
    }

private:
    DiskManager& diskManager_;
    LogManager& logManager_;
    CheckpointControl* checkpointControl_;
    bool forceFullScan_;
    RedoPolicy redoPolicy_;
    std::vector<RecoveryTransactionEntry> analysisTransactions_;
    std::vector<RecoveryTransactionEntry> recoveredTransactions_;
};

class RecoveryCoordinator final : public PageRecoveryHook {
public:
    RecoveryCoordinator(
        DiskManager& diskManager,
        LogManager& logManager,
        TransactionId nextTransactionId = INVALID_TRANSACTION_ID,
        WalUpdateMode updateMode = WalUpdateMode::FullPage);

    void attachBufferPool(BufferPoolManager& bufferPool) noexcept;
    [[nodiscard]] TransactionId beginTransaction();
    // Binding is operation-local physical ownership, not transaction-table ownership.
    // Production binds only under the database-wide exclusive writer lease.
    void bindTransaction(TransactionId id);
    void beginMutation(TransactionId id);
    void prepareTransaction(TransactionId id);
    void commitTransaction(TransactionId id);
    void rollbackTransaction(TransactionId id);
    [[nodiscard]] std::vector<CheckpointTransactionEntry> checkpointTransactions() const;
    [[nodiscard]] std::vector<RecoveryTransactionEntry> transactionSnapshot() const;
    [[nodiscard]] bool checkpointSafe() const;
    void beginStatement();
    // Finalize a successful statement inside a still-active transaction. This
    // logs resident changes without COMMIT, WAL force, or database-page force.
    void prepareStatement();
    void commitStatement();
    void rollbackStatement();

    [[nodiscard]] bool hasActiveStatement() const noexcept { return contextCount_.load() != 0; }
    [[nodiscard]] bool rollbackActive() const noexcept { return rollbackActive_; }
    [[nodiscard]] TransactionId activeTransactionId() const noexcept;
    [[nodiscard]] bool hasMaterializedWalBegin(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    [[nodiscard]] Lsn lastLsn(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    [[nodiscard]] std::size_t touchedPageCount(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    [[nodiscard]] std::uint64_t transactionWalBytes(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    [[nodiscard]] std::uint64_t originalBeforeImageBytes(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    // Accounted context/PageState storage, excluding std::map allocator overhead.
    [[nodiscard]] std::uint64_t transactionRecoveryBytes(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    [[nodiscard]] std::uint64_t peakTransactionRecoveryBytes(TransactionId id = INVALID_TRANSACTION_ID) const noexcept;
    [[nodiscard]] const RecoveryStats& lastRollbackStats() const noexcept {
        return lastRollbackStats_;
    }
    [[nodiscard]] TransactionId nextTransactionId() const noexcept {
        std::lock_guard lock(contextsMutex_);
        return nextTransactionId_;
    }
    [[nodiscard]] WalUpdateMode updateMode() const noexcept { return updateMode_; }
    [[nodiscard]] const TransactionRuntimeStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = {}; }

    void notePageWriteIntent(PageId pageId, const DiskManager::Page& before) override;
    void prepareForPhysicalPageAppend() override;
    void prepareForPhysicalPageAppend(TransactionId id);
    [[nodiscard]] bool needsPreparation() const noexcept override { return activeSignal_.load(); }
    [[nodiscard]] Lsn preparePageForWrite(
        PageId pageId,
        DiskManager::Page& after) override;

private:
    struct PageState {
        bool beforeExisted = false;
        DiskManager::Page before{};
        Lsn beforePageLsn = INVALID_LSN;
        std::optional<DiskManager::Page> latestAfter;
        std::array<bool, database_format::PAGE_SIZE> touchedOffsets{};
        Lsn latestLsn = INVALID_LSN;
    };

    struct RecoveryTransactionContext : RecoveryTransactionEntry {
        std::map<PageId, PageState> pages;
        std::uint64_t walBytes = 0;
        std::uint64_t peakRecoveryBytes = 0;
        bool safeBoundary = true;
    };

    DiskManager& diskManager_;
    LogManager& logManager_;
    BufferPoolManager* bufferPool_ = nullptr;
    TransactionId nextTransactionId_ = 1;
    WalUpdateMode updateMode_ = WalUpdateMode::FullPage;
    std::map<TransactionId, RecoveryTransactionContext> contexts_;
    // Non-owning handle into contexts_; never retained across erase/rebind.
    RecoveryTransactionContext* active_ = nullptr;
    mutable std::recursive_mutex contextsMutex_;
    std::atomic<std::size_t> contextCount_{0};
    std::atomic<bool> activeSignal_{false};
    std::atomic<bool> rollbackActive_{false};
    TransactionRuntimeStats stats_{};
    RecoveryStats lastRollbackStats_{};

    void ensureBeginLogged(RecoveryTransactionContext& context);
    [[nodiscard]] Lsn appendTransactionRecord(
        RecoveryTransactionContext& context,
        LogRecordType type,
        std::vector<std::byte> payload = {});
    void requireNoPins() const;
};

// Tests activate failpoints through MINIDB_FAILPOINT. Production is a no-op.
void recoveryFailPoint(std::string_view name);

} // namespace minidb
