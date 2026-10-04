#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/recovery.hpp"
#include "minidb/slotted_page.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using minidb::test::require;
constexpr minidb::PageId DATA_PAGE = 1;

void initialize(const std::string& path) {
    minidb::DiskManager disk(path);
    require(disk.appendPage() == DATA_PAGE, "Unexpected data page ID");
    const auto heapMetadata = disk.appendPage();
    minidb::DiskManager::Page page{};
    minidb::SlottedPageView::initialize(
        page, DATA_PAGE, disk.pageCount(), heapMetadata);
    disk.writePage(DATA_PAGE, page);
    disk.sync();
}

void setByte(minidb::BufferPoolManager& pool, std::size_t offset, unsigned char value) {
    auto page = pool.fetchPageWrite(DATA_PAGE);
    require(page.has_value(), "Could not fetch writable page");
    page->data()[offset] = static_cast<std::byte>(value);
}

void requireWinner(minidb::DiskManager& disk) {
    minidb::DiskManager::Page page{};
    disk.readPage(DATA_PAGE, page);
    require(page[100] == std::byte{0x11} && page[200] == std::byte{0x22}
                && page[300] == std::byte{0},
            "Rollback did not preserve the complete NO-FORCE winner image");
    require(minidb::isValidLsn(minidb::readPersistentPageLsn(page)),
            "Compensated page does not carry its CLR PageLSN");
}

void prepareWinnerAndLoser(
    minidb::DiskManager& disk,
    minidb::LogManager& log,
    minidb::RecoveryCoordinator& recovery,
    minidb::BufferPoolManager& pool) {
    recovery.beginStatement();
    setByte(pool, 100, 0x11);
    setByte(pool, 200, 0x22);
    recovery.commitStatement();
    minidb::DiskManager::Page physical{};
    disk.readPage(DATA_PAGE, physical);
    require(physical[100] == std::byte{0} && physical[200] == std::byte{0},
            "Setup unexpectedly forced the committed winner page");

    recovery.beginStatement();
    const auto transaction = recovery.activeTransactionId();
    const auto syncs = log.stats().fsyncCalls;
    require(!recovery.hasMaterializedWalBegin() && recovery.transactionWalBytes() == 0,
            "BEGIN was not lazy");
    for (unsigned char value = 0x31; value <= 0x33; ++value) {
        setByte(pool, 100, value);
        setByte(pool, 300, value);
        recovery.prepareStatement();
        require(recovery.activeTransactionId() == transaction
                    && recovery.hasMaterializedWalBegin()
                    && minidb::isValidLsn(recovery.lastLsn()),
                "Statement boundary replaced the transaction chain");
    }
    require(log.stats().fsyncCalls == syncs,
            "Preparing statements unnecessarily forced WAL");
    require(recovery.touchedPageCount() == 1
                && recovery.originalBeforeImageBytes() == minidb::DiskManager::PAGE_SIZE
                && recovery.transactionRecoveryBytes() > recovery.originalBeforeImageBytes()
                && recovery.peakTransactionRecoveryBytes() == recovery.transactionRecoveryBytes()
                && recovery.transactionWalBytes() > 0,
            "Transaction recovery diagnostics are inconsistent");
    minidb::Lsn previous = minidb::INVALID_LSN;
    std::size_t updates = 0;
    for (const auto& record : log.scan().records) {
        if (record.transactionId != transaction) continue;
        require(record.prevLsn == previous, "WAL chain broke across statements");
        previous = record.lsn;
        require(record.type != minidb::LogRecordType::Commit,
                "Statement boundary emitted an intermediate COMMIT");
        if (record.type != minidb::LogRecordType::Begin) ++updates;
    }
    require(updates == 3, "Each modified statement must prepare a new update");
}

void testLiveRollbackAndCacheInvalidation() {
    for (const auto mode : {minidb::WalUpdateMode::FullPage,
                            minidb::WalUpdateMode::ByteRange,
                            minidb::WalUpdateMode::Adaptive}) {
        minidb::test::TemporaryDatabase database("runtime_clr_winner");
        const auto path = database.path().string();
        initialize(path);
        {
            minidb::DiskManager disk(path);
            minidb::LogManager log(minidb::walPathForDatabase(path));
            minidb::RecoveryCoordinator recovery(disk, log, minidb::INVALID_TRANSACTION_ID, mode);
            minidb::BufferPoolManager pool(disk, 8, 2, &log, &recovery);
            recovery.attachBufferPool(pool);
            prepareWinnerAndLoser(disk, log, recovery, pool);
            recovery.rollbackStatement();
            requireWinner(disk);
            require(!recovery.hasActiveStatement() && !pool.isResident(DATA_PAGE),
                    "Rollback left active ownership or a stale buffer image");
            const auto& stats = recovery.lastRollbackStats();
            require(stats.clrsAppended == 3 && stats.undoUserRecordsVisited == 3
                        && stats.durableAbortObserved && stats.undoPageWrites == 3,
                    "Live rollback did not use the restartable CLR protocol");
            const auto records = log.scan().records;
            require(records.back().type == minidb::LogRecordType::Abort
                        && log.durableLsn() >= records.back().lsn,
                    "Rollback returned before durable ABORT");
            auto page = pool.fetchPageRead(DATA_PAGE);
            require(page->data()[100] == std::byte{0x11}
                        && page->data()[200] == std::byte{0x22},
                    "Next read observed stale loser bytes");
        }
        minidb::DiskManager disk(path);
        minidb::LogManager log(minidb::walPathForDatabase(path));
        static_cast<void>(minidb::RecoveryManager(disk, log).recover());
        requireWinner(disk);
    }
}

void testReadOnlyEndsGenerateNoWal() {
    minidb::test::TemporaryDatabase database("runtime_clr_readonly");
    minidb::DiskManager disk(database.path().string());
    minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
    minidb::RecoveryCoordinator recovery(disk, log);
    const auto initial = log.stats();
    recovery.beginStatement();
    const auto firstId = recovery.activeTransactionId();
    recovery.prepareStatement();
    recovery.commitStatement();
    recovery.beginStatement();
    require(recovery.activeTransactionId() > firstId, "Read-only IDs are not monotonic");
    recovery.rollbackStatement();
    require(log.stats().recordsAppended == initial.recordsAppended
                && log.stats().fsyncCalls == initial.fsyncCalls,
            "Read-only transaction completion generated WAL or forced it");
}

void testLiveRollbackCrashMatrix() {
    for (const auto* failpoint : {
             "recovery_before_clr_append", "recovery_after_clr_append",
             "recovery_after_clr_wal_force", "recovery_after_compensation_page_write",
             "recovery_after_final_clr", "recovery_before_abort_append",
             "recovery_after_abort_append", "recovery_after_abort_fsync",
         }) {
        minidb::test::TemporaryDatabase database("runtime_clr_crash");
        const auto path = database.path().string();
        initialize(path);
        const auto child = ::fork();
        if (child < 0) throw std::runtime_error("fork failed");
        if (child == 0) {
            minidb::DiskManager disk(path);
            minidb::LogManager log(minidb::walPathForDatabase(path));
            minidb::RecoveryCoordinator recovery(
                disk, log, minidb::INVALID_TRANSACTION_ID, minidb::WalUpdateMode::ByteRange);
            minidb::BufferPoolManager pool(disk, 8, 2, &log, &recovery);
            recovery.attachBufferPool(pool);
            prepareWinnerAndLoser(disk, log, recovery, pool);
            ::setenv("MINIDB_FAILPOINT", failpoint, 1);
            recovery.rollbackStatement();
            ::_exit(90);
        }
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status)
                    && WEXITSTATUS(status) == 86,
                std::string("Live rollback missed failpoint: ") + failpoint);
        minidb::DiskManager disk(path);
        minidb::LogManager log(
            minidb::walPathForDatabase(path), minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        static_cast<void>(minidb::RecoveryManager(disk, log).recover());
        requireWinner(disk);
        require(log.scan().records.back().type == minidb::LogRecordType::Abort,
                "Interrupted live rollback did not converge to ABORT");
    }
}

void testLiveRollbackTruncatesNewPages() {
    minidb::test::TemporaryDatabase database("runtime_clr_truncation");
    minidb::DiskManager disk(database.path().string());
    minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
    minidb::RecoveryCoordinator recovery(disk, log);
    minidb::BufferPoolManager pool(disk, 1, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    recovery.beginStatement();
    for (unsigned char value = 1; value <= 4; ++value) {
        auto page = pool.newPageWrite();
        require(page.has_value(), "Could not append transaction page");
        page->data()[100] = static_cast<std::byte>(value);
        page->drop();
        recovery.prepareStatement();
    }
    require(pool.stats().dirtyEvictions >= 3, "Truncation setup did not exercise STEAL");
    recovery.rollbackStatement();
    require(disk.pageCount() == 1 && pool.residentPageCount() == 0
                && recovery.lastRollbackStats().pagesTruncated == 4
                && recovery.lastRollbackStats().clrsAppended == 0,
            "New pages were not removed at the transaction BEGIN boundary");
}

} // namespace

int main() {
    try {
        testLiveRollbackAndCacheInvalidation();
        testReadOnlyEndsGenerateNoWal();
        testLiveRollbackCrashMatrix();
        testLiveRollbackTruncatesNewPages();
        std::cout << "runtime_clr_rollback_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime_clr_rollback_test failed: " << error.what() << '\n';
        return 1;
    }
}
