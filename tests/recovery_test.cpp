#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "test_utils.hpp"

#include <filesystem>
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

} // namespace

int main() {
    try {
        testCommitIsNoForceAndRedoSurvivesReopen();
        testStealLoserUndoAndTruncation();
        testExplicitRollbackAndZeroMutation();
        std::cout << "recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
