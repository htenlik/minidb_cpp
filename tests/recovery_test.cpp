#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

using minidb::test::require;

class RecoveryFixture {
public:
    explicit RecoveryFixture(const char* name)
        : database(name), wal(minidb::walPathForDatabase(database.path().string())) {}
    ~RecoveryFixture() {
        std::error_code error;
        std::filesystem::remove(wal, error);
    }
    minidb::test::TemporaryDatabase database;
    std::string wal;
};

std::byte diskByte(minidb::DiskManager& disk, minidb::PageId page, std::size_t offset) {
    minidb::DiskManager::Page bytes{};
    disk.readPage(page, bytes);
    return bytes[offset];
}

void testCommitIsNoForceAndRedoSurvivesReopen(minidb::WalUpdateMode mode) {
    RecoveryFixture fixture("recovery_commit_redo");
    minidb::PageId pageId = minidb::INVALID_PAGE_ID;
    {
        minidb::DiskManager disk(fixture.database.path().string());
        minidb::LogManager log(fixture.wal);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID, mode);
        minidb::BufferPoolManager pool(disk, 2, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        auto page = pool.newPageWrite();
        pageId = page->pageId();
        page->data()[77] = std::byte{0x77};
        page->drop();
        coordinator.commitStatement();
        require(diskByte(disk, pageId, 77) == std::byte{0},
                "Commit forced a dirty database page");
        // Simulate loss of the no-force buffer without running its destructor.
        pool.discardPageForRecovery(pageId);
    }
    {
        minidb::DiskManager disk(fixture.database.path().string());
        minidb::LogManager log(fixture.wal);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.pagesRedone == 1 && diskByte(disk, pageId, 77) == std::byte{0x77},
                "Recovery did not REDO committed no-force page update");
    }
}

void testStealLoserUndoAndTruncation(minidb::WalUpdateMode mode) {
    RecoveryFixture fixture("recovery_steal_undo");
    minidb::PageId existing = minidb::INVALID_PAGE_ID;
    std::uint64_t startCount = 0;
    {
        minidb::DiskManager disk(fixture.database.path().string());
        existing = disk.appendPage();
        startCount = disk.pageCount();
        minidb::LogManager log(fixture.wal);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID, mode);
        minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        {
            auto page = pool.fetchPageWrite(existing);
            page->data()[10] = std::byte{0xAA};
        }
        minidb::PageId appended = minidb::INVALID_PAGE_ID;
        {
            auto page = pool.newPageWrite(); // evicts and steals existing
            appended = page->pageId();
            page->data()[11] = std::byte{0xBB};
        }
        require(pool.flushPage(appended), "Could not flush appended loser page");
        require(diskByte(disk, existing, 10) == std::byte{0xAA},
                "STEAL setup did not write the loser image");
    }
    {
        minidb::DiskManager disk(fixture.database.path().string());
        minidb::LogManager log(fixture.wal);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.loserTransactions == 1 && stats.pagesUndone == 1,
                "Recovery did not identify and undo the loser");
        require(disk.pageCount() == startCount && diskByte(disk, existing, 10) == std::byte{0},
                "Loser undo did not restore bytes and truncate appended pages");
        const auto second = minidb::RecoveryManager(disk, log).recover();
        require(second.loserTransactions == 0 && second.pagesUndone == 0,
                "Repeated recovery was not idempotent");
    }
}

void testExplicitRollbackAndZeroMutation() {
    RecoveryFixture fixture("recovery_rollback_zero");
    minidb::DiskManager disk(fixture.database.path().string());
    const auto pageId = disk.appendPage();
    minidb::LogManager log(fixture.wal);
    minidb::RecoveryCoordinator coordinator(disk, log);
    minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
    coordinator.attachBufferPool(pool);

    const auto recordsBefore = log.scan().records.size();
    coordinator.beginStatement();
    coordinator.commitStatement();
    require(log.scan().records.size() == recordsBefore,
            "Zero-mutation statement emitted WAL");

    coordinator.beginStatement();
    {
        auto page = pool.fetchPageWrite(pageId);
        page->data()[5] = std::byte{0x55};
    }
    coordinator.rollbackStatement();
    require(diskByte(disk, pageId, 5) == std::byte{0}
                && !pool.isResident(pageId),
            "Explicit rollback did not restore and invalidate the page");
    const auto records = log.scan().records;
    require(records.size() == recordsBefore,
            "Unflushed in-memory mutation emitted BEGIN/ABORT WAL");
}

void appendRaw(const std::string& path, std::size_t count) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    const std::vector<std::byte> bytes(count, std::byte{0xCC});
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void testTailRepairAndChainValidation() {
    RecoveryFixture fixture("recovery_tail_repair");
    {
        minidb::LogManager log(fixture.wal);
        const auto begin = log.append(minidb::LogRecord{
            minidb::LogRecordType::Begin, 1, minidb::INVALID_LSN,
            minidb::encodeBeginLogPayload({1})});
        const auto commit = log.append(minidb::LogRecord{
            minidb::LogRecordType::Commit, 1, begin, {}});
        log.flushUpTo(commit);
    }
    appendRaw(fixture.wal, 17);
    {
        minidb::DiskManager disk(fixture.database.path().string());
        minidb::LogManager log(fixture.wal);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.repairedTail && stats.tailBytesTruncated == 17,
                "Startup recovery did not report exact incomplete-tail repair");
        minidb::RecoveryCoordinator coordinator(disk, log);
        coordinator.beginStatement();
        coordinator.commitStatement();
        require(!log.scan().truncatedTail, "WAL could not continue after tail repair");
    }

    RecoveryFixture corrupt("recovery_chain_corrupt");
    {
        minidb::LogManager log(corrupt.wal);
        static_cast<void>(log.append(minidb::LogRecord{
            minidb::LogRecordType::Begin, 1, minidb::INVALID_LSN,
            minidb::encodeBeginLogPayload({1})}));
        static_cast<void>(log.append(minidb::LogRecord{
            minidb::LogRecordType::Begin, 2, minidb::INVALID_LSN,
            minidb::encodeBeginLogPayload({1})}));
        log.flushAll();
    }
    minidb::DiskManager disk(corrupt.database.path().string());
    minidb::LogManager log(corrupt.wal);
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::RecoveryManager(disk, log).recover()); },
        "Recovery accepted overlapping active transaction chains");
}

void testPageZeroCommitAndRollback(minidb::WalUpdateMode mode) {
    RecoveryFixture fixture("recovery_page_zero");
    minidb::DiskManager disk(fixture.database.path().string());
    const auto rootA = disk.appendPage();
    const auto rootB = disk.appendPage();
    minidb::LogManager log(fixture.wal);
    minidb::RecoveryCoordinator coordinator(
        disk, log, minidb::INVALID_TRANSACTION_ID, mode);
    minidb::BufferPoolManager pool(disk, 2, 2, &log, &coordinator);
    coordinator.attachBufferPool(pool);
    minidb::DatabaseMetadataManager metadata(disk, coordinator, log);

    coordinator.beginStatement();
    metadata.updateCatalogRootPageId(rootA);
    coordinator.commitStatement();
    require(disk.databaseHeader().catalogRootPageId == rootA,
            "Committed catalog-root page-0 update was lost");

    coordinator.beginStatement();
    metadata.updateCatalogRootPageId(rootB);
    metadata.updateFreeListRootPageId(rootB);
    coordinator.rollbackStatement();
    require(disk.databaseHeader().catalogRootPageId == rootA
                && disk.databaseHeader().freeListRootPageId == minidb::INVALID_PAGE_ID,
            "Rollback did not restore the complete original page-0 image");

    minidb::PageAllocator allocator(pool, disk, &metadata);
    allocator.validate();
}

void testRepeatedDeltaWritesPreserveOriginalUndoAndFinalRedo() {
    RecoveryFixture loserFixture("recovery_repeated_delta_loser");
    minidb::PageId loserPage = minidb::INVALID_PAGE_ID;
    {
        minidb::DiskManager disk(loserFixture.database.path().string());
        loserPage = disk.appendPage();
        minidb::LogManager log(loserFixture.wal);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID,
            minidb::WalUpdateMode::ByteRange);
        minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        {
            auto page = pool.fetchPageWrite(loserPage);
            page->data()[10] = std::byte{0xAA};
        }
        require(pool.flushPage(loserPage), "Could not STEAL first delta state");
        {
            auto page = pool.fetchPageWrite(loserPage);
            page->data()[10] = std::byte{0};
            page->data()[20] = std::byte{0xBB};
        }
        require(pool.flushPage(loserPage), "Could not STEAL second delta state");
        require(diskByte(disk, loserPage, 10) == std::byte{0}
                    && diskByte(disk, loserPage, 20) == std::byte{0xBB},
                "Repeated-delta loser setup did not reach the latest state");
        const auto records = log.scan().records;
        const auto latest = minidb::decodePageDeltaUpdateLogPayload(
            records[records.size() - 1].payload);
        require(latest.ranges.size() == 2
                    && latest.ranges[0].offset == 10
                    && latest.ranges[0].beforeBytes == latest.ranges[0].afterBytes,
                "Latest delta omitted a reverted but transaction-touched byte");
    }
    {
        minidb::DiskManager disk(loserFixture.database.path().string());
        minidb::LogManager log(loserFixture.wal);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.pagesUndone == 1
                    && diskByte(disk, loserPage, 10) == std::byte{0}
                    && diskByte(disk, loserPage, 20) == std::byte{0},
                "Repeated-delta loser did not restore the full original page state");
    }

    RecoveryFixture winnerFixture("recovery_repeated_delta_winner");
    minidb::PageId winnerPage = minidb::INVALID_PAGE_ID;
    {
        minidb::DiskManager disk(winnerFixture.database.path().string());
        winnerPage = disk.appendPage();
        minidb::LogManager log(winnerFixture.wal);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID,
            minidb::WalUpdateMode::ByteRange);
        minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        {
            auto page = pool.fetchPageWrite(winnerPage);
            page->data()[10] = std::byte{0xAA};
        }
        require(pool.flushPage(winnerPage), "Could not log first winner delta state");
        {
            auto page = pool.fetchPageWrite(winnerPage);
            page->data()[10] = std::byte{0};
            page->data()[20] = std::byte{0xCC};
        }
        coordinator.commitStatement();
        pool.discardPageForRecovery(winnerPage);
    }
    {
        minidb::DiskManager disk(winnerFixture.database.path().string());
        minidb::LogManager log(winnerFixture.wal);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.pagesRedone == 2
                    && diskByte(disk, winnerPage, 10) == std::byte{0}
                    && diskByte(disk, winnerPage, 20) == std::byte{0xCC},
                "Repeated-delta winner REDO lost a reverted or latest byte");
    }
}

void commitOneByte(
    RecoveryFixture& fixture,
    minidb::PageId pageId,
    std::size_t offset,
    std::byte value,
    minidb::WalUpdateMode mode) {
    minidb::DiskManager disk(fixture.database.path().string());
    minidb::LogManager log(fixture.wal);
    const auto startup = minidb::RecoveryManager(disk, log).recover();
    minidb::RecoveryCoordinator coordinator(
        disk, log, startup.nextTransactionId, mode);
    minidb::BufferPoolManager pool(disk, 2, 2, &log, &coordinator);
    coordinator.attachBufferPool(pool);
    coordinator.beginStatement();
    {
        auto page = pool.fetchPageWrite(pageId);
        page->data()[offset] = value;
    }
    coordinator.commitStatement();
    pool.discardPageForRecovery(pageId);
}

void testMixedFullPageAndDeltaHistory() {
    RecoveryFixture fixture("recovery_mixed_update_history");
    minidb::PageId pageId = minidb::INVALID_PAGE_ID;
    {
        minidb::DiskManager disk(fixture.database.path().string());
        pageId = disk.appendPage();
    }
    commitOneByte(
        fixture, pageId, 1, std::byte{0x11}, minidb::WalUpdateMode::FullPage);
    commitOneByte(
        fixture, pageId, 2, std::byte{0x22}, minidb::WalUpdateMode::ByteRange);
    commitOneByte(
        fixture, pageId, 3, std::byte{0x33}, minidb::WalUpdateMode::FullPage);
    commitOneByte(
        fixture, pageId, 4, std::byte{0x44}, minidb::WalUpdateMode::Adaptive);

    minidb::DiskManager disk(fixture.database.path().string());
    minidb::LogManager log(fixture.wal);
    static_cast<void>(minidb::RecoveryManager(disk, log).recover());
    require(diskByte(disk, pageId, 1) == std::byte{0x11}
                && diskByte(disk, pageId, 2) == std::byte{0x22}
                && diskByte(disk, pageId, 3) == std::byte{0x33}
                && diskByte(disk, pageId, 4) == std::byte{0x44},
            "Mixed full-page/delta history did not recover its final state");
    bool sawFullPage = false;
    bool sawDelta = false;
    for (const auto& record : log.scan().records) {
        sawFullPage = sawFullPage || record.type == minidb::LogRecordType::PageUpdate;
        sawDelta = sawDelta || record.type == minidb::LogRecordType::PageDeltaUpdate;
    }
    require(sawFullPage && sawDelta,
            "Mixed-history fixture did not retain both update encodings");
}

void runMixedAdaptiveTransactionCrash(bool committed) {
    RecoveryFixture fixture(committed
        ? "recovery_adaptive_mixed_winner" : "recovery_adaptive_mixed_loser");
    minidb::PageId sparsePage = minidb::INVALID_PAGE_ID;
    minidb::PageId densePage = minidb::INVALID_PAGE_ID;
    {
        minidb::DiskManager disk(fixture.database.path().string());
        sparsePage = disk.appendPage();
        densePage = disk.appendPage();
        minidb::LogManager log(fixture.wal);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID,
            minidb::WalUpdateMode::Adaptive);
        minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        {
            auto page = pool.fetchPageWrite(sparsePage);
            page->data()[31] = std::byte{0x31};
        }
        {
            auto page = pool.fetchPageWrite(densePage);
            std::fill(page->data().begin(), page->data().end(), std::byte{0xD2});
        }
        require(pool.flushPage(densePage),
                "Could not STEAL dense adaptive page before crash");
        const auto transactionStats = coordinator.stats();
        require(transactionStats.adaptiveDeltaSelections == 1
                    && transactionStats.adaptiveFullPageSelections == 1
                    && transactionStats.bytesActuallyChosen
                        <= transactionStats.bytesIfFullPage
                    && transactionStats.bytesActuallyChosen
                        <= transactionStats.bytesIfDelta,
                "One adaptive transaction did not choose both physical encodings");
        if (committed) coordinator.commitStatement();
        const auto records = log.scan().records;
        bool sawFull = false;
        bool sawDelta = false;
        for (const auto& record : records) {
            sawFull = sawFull || record.type == minidb::LogRecordType::PageUpdate;
            sawDelta = sawDelta || record.type == minidb::LogRecordType::PageDeltaUpdate;
        }
        require(sawFull && sawDelta,
                "Adaptive mixed transaction WAL lacks one selected record type");
        if (pool.isResident(densePage)) pool.discardPageForRecovery(densePage);
    }
    {
        minidb::DiskManager disk(fixture.database.path().string());
        minidb::LogManager log(fixture.wal);
        const auto recovery = minidb::RecoveryManager(disk, log).recover();
        const auto expectedSparse = committed ? std::byte{0x31} : std::byte{0};
        const auto expectedDense = committed ? std::byte{0xD2} : std::byte{0};
        require(diskByte(disk, sparsePage, 31) == expectedSparse
                    && diskByte(disk, densePage, 0) == expectedDense
                    && diskByte(disk, densePage, 4095) == expectedDense,
                "Mixed adaptive transaction did not recover atomically");
        require(committed ? recovery.pagesRedone == 2 : recovery.pagesUndone == 2,
                "Mixed adaptive recovery processed the wrong page count");
    }
}

} // namespace

int main() {
    try {
        for (const auto mode : {minidb::WalUpdateMode::FullPage,
                                minidb::WalUpdateMode::ByteRange,
                                minidb::WalUpdateMode::Adaptive}) {
            testCommitIsNoForceAndRedoSurvivesReopen(mode);
            testStealLoserUndoAndTruncation(mode);
            testPageZeroCommitAndRollback(mode);
        }
        testExplicitRollbackAndZeroMutation();
        testTailRepairAndChainValidation();
        testRepeatedDeltaWritesPreserveOriginalUndoAndFinalRedo();
        testMixedFullPageAndDeltaHistory();
        runMixedAdaptiveTransactionCrash(false);
        runMixedAdaptiveTransactionCrash(true);
        std::cout << "recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
