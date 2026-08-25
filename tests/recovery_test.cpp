#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "test_utils.hpp"

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

void testCommitIsNoForceAndRedoSurvivesReopen() {
    RecoveryFixture fixture("recovery_commit_redo");
    minidb::PageId pageId = minidb::INVALID_PAGE_ID;
    {
        minidb::DiskManager disk(fixture.database.path().string());
        minidb::LogManager log(fixture.wal);
        minidb::RecoveryCoordinator coordinator(disk, log);
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

void testStealLoserUndoAndTruncation() {
    RecoveryFixture fixture("recovery_steal_undo");
    minidb::PageId existing = minidb::INVALID_PAGE_ID;
    std::uint64_t startCount = 0;
    {
        minidb::DiskManager disk(fixture.database.path().string());
        existing = disk.appendPage();
        startCount = disk.pageCount();
        minidb::LogManager log(fixture.wal);
        minidb::RecoveryCoordinator coordinator(disk, log);
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

void testPageZeroCommitAndRollback() {
    RecoveryFixture fixture("recovery_page_zero");
    minidb::DiskManager disk(fixture.database.path().string());
    const auto rootA = disk.appendPage();
    const auto rootB = disk.appendPage();
    minidb::LogManager log(fixture.wal);
    minidb::RecoveryCoordinator coordinator(disk, log);
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

} // namespace

int main() {
    try {
        testCommitIsNoForceAndRedoSurvivesReopen();
        testStealLoserUndoAndTruncation();
        testExplicitRollbackAndZeroMutation();
        testTailRepairAndChainValidation();
        testPageZeroCommitAndRollback();
        std::cout << "recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
