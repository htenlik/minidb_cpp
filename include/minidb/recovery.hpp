#pragma once

#include "minidb/page_recovery.hpp"
#include "minidb/checkpoint_types.hpp"
#include "minidb/recovery_log.hpp"

#include <array>
#include <cstdint>
#include <map>
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

struct RecoveryStats {
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
    std::uint64_t legacyRedoRecords = 0;
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
    WalOffset recoveryStartOffset = wal_file_layout::HEADER_SIZE;
    std::uint64_t walBytesSkipped = 0;
    std::uint64_t walBytesScanned = 0;
    bool fullScanFallback = true;
    std::uint64_t checkpointValidationFailures = 0;
    std::uint64_t checkpointGeneration = 0;
    CheckpointId highestCheckpointId = INVALID_CHECKPOINT_ID;
    TransactionId nextTransactionId = 1;
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

private:
    DiskManager& diskManager_;
    LogManager& logManager_;
    CheckpointControl* checkpointControl_;
    bool forceFullScan_;
    RedoPolicy redoPolicy_;
};

class RecoveryCoordinator final : public PageRecoveryHook {
public:
    RecoveryCoordinator(
        DiskManager& diskManager,
        LogManager& logManager,
        TransactionId nextTransactionId = INVALID_TRANSACTION_ID,
        WalUpdateMode updateMode = WalUpdateMode::FullPage);

    void attachBufferPool(BufferPoolManager& bufferPool) noexcept;
    void beginStatement();
    void commitStatement();
    void rollbackStatement();

    [[nodiscard]] bool hasActiveStatement() const noexcept { return active_.has_value(); }
    [[nodiscard]] bool rollbackActive() const noexcept { return rollbackActive_; }
    [[nodiscard]] TransactionId activeTransactionId() const noexcept;
    [[nodiscard]] TransactionId nextTransactionId() const noexcept { return nextTransactionId_; }
    [[nodiscard]] WalUpdateMode updateMode() const noexcept { return updateMode_; }
    [[nodiscard]] const TransactionRuntimeStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = {}; }

    void notePageWriteIntent(PageId pageId, const DiskManager::Page& before) override;
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

    struct ActiveStatement {
        TransactionId transactionId = INVALID_TRANSACTION_ID;
        std::uint64_t startPageCount = 0;
        Lsn beginLsn = INVALID_LSN;
        Lsn previousLsn = INVALID_LSN;
        std::map<PageId, PageState> pages;
    };

    DiskManager& diskManager_;
    LogManager& logManager_;
    BufferPoolManager* bufferPool_ = nullptr;
    TransactionId nextTransactionId_ = 1;
    WalUpdateMode updateMode_ = WalUpdateMode::FullPage;
    std::optional<ActiveStatement> active_;
    bool rollbackActive_ = false;
    TransactionRuntimeStats stats_{};

    void ensureBeginLogged();
    [[nodiscard]] Lsn appendTransactionRecord(
        LogRecordType type,
        std::vector<std::byte> payload = {});
    void requireNoPins() const;
};

// Tests activate failpoints through MINIDB_FAILPOINT. Production is a no-op.
void recoveryFailPoint(std::string_view name);

} // namespace minidb
