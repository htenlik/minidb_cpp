#pragma once

#include "minidb/page_recovery.hpp"
#include "minidb/recovery_log.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

namespace minidb {

class BufferPoolManager;
class LogManager;

struct RecoveryStats {
    std::uint64_t recordsAnalyzed = 0;
    std::uint64_t committedTransactions = 0;
    std::uint64_t abortedTransactions = 0;
    std::uint64_t loserTransactions = 0;
    std::uint64_t pagesRedone = 0;
    std::uint64_t pagesUndone = 0;
    std::uint64_t pagesTruncated = 0;
    bool repairedTail = false;
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager& diskManager, LogManager& logManager)
        : diskManager_(diskManager), logManager_(logManager) {}

    [[nodiscard]] RecoveryStats recover();

private:
    DiskManager& diskManager_;
    LogManager& logManager_;
};

class RecoveryCoordinator final : public PageRecoveryHook {
public:
    RecoveryCoordinator(DiskManager& diskManager, LogManager& logManager);

    void attachBufferPool(BufferPoolManager& bufferPool) noexcept;
    void beginStatement();
    void commitStatement();
    void rollbackStatement();

    [[nodiscard]] bool hasActiveStatement() const noexcept { return active_.has_value(); }
    [[nodiscard]] TransactionId activeTransactionId() const noexcept;
    [[nodiscard]] TransactionId nextTransactionId() const noexcept { return nextTransactionId_; }

    void notePageWriteIntent(PageId pageId, const DiskManager::Page& before) override;
    [[nodiscard]] Lsn preparePageForWrite(
        PageId pageId,
        const DiskManager::Page& after) override;

private:
    struct PageState {
        bool beforeExisted = false;
        DiskManager::Page before{};
        std::optional<DiskManager::Page> latestAfter;
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
    std::optional<ActiveStatement> active_;

    void ensureBeginLogged();
    [[nodiscard]] Lsn appendTransactionRecord(
        LogRecordType type,
        std::vector<std::byte> payload = {});
    void requireNoPins() const;
};

// Tests activate failpoints through MINIDB_FAILPOINT. Production is a no-op.
void recoveryFailPoint(std::string_view name);

} // namespace minidb
