#include "minidb/database_server.hpp"
#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/segmented_wal.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {

using minidb::test::require;

minidb::net::ServerConfig config(
    std::uint32_t segmentBytes,
    std::size_t frames,
    minidb::WalUpdateMode mode) {
    minidb::net::ServerConfig result;
    result.port = 0;
    result.bufferFrames = frames;
    result.lruK = 2;
    result.checkpointWalBytes = 0;
    result.checkpointStatements = 0;
    result.walSegmentBytes = segmentBytes;
    result.walUpdateMode = mode;
    return result;
}

std::size_t rowCount(minidb::net::DatabaseServer& server, const std::string& table) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM " + table)).rows.size();
}

void testCrossSegmentCommitAndReopen(minidb::WalUpdateMode mode) {
    minidb::test::TemporaryDatabase database("wal_cross_segment_commit");
    constexpr std::uint32_t SEGMENT_BYTES = 12U * 1024U;
    const auto path = database.path().string();
    {
        minidb::net::DatabaseServer server(path, config(SEGMENT_BYTES, 2, mode));
        static_cast<void>(server.sqlEngine().execute(
            "CREATE TABLE docs (id UINT32 PRIMARY KEY, body VARCHAR(3000) NOT NULL)"));
        for (std::uint32_t key = 0; key < 8; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO docs VALUES (" + std::to_string(key) + ", '"
                + std::string(2500, static_cast<char>('a' + key)) + "')"));
        }
    }

    minidb::SegmentedWalStorage storage(
        minidb::segmentedWalPathForDatabase(path), SEGMENT_BYTES, false);
    std::map<minidb::TransactionId, std::set<minidb::WalSegmentId>> locations;
    for (const auto& record : storage.scan().records) {
        for (const auto& segment : storage.segments()) {
            if (record.lsn >= segment.header.startLsn
                && record.lsn < segment.logicalEndLsn) {
                locations[record.transactionId].insert(segment.header.segmentId);
                break;
            }
        }
    }
    require(std::any_of(locations.begin(), locations.end(), [](const auto& entry) {
                return entry.first != minidb::INVALID_TRANSACTION_ID
                    && entry.second.size() > 1;
            }),
            "No committed transaction crossed the forced segment boundary");
    {
        minidb::net::DatabaseServer reopened(path, config(SEGMENT_BYTES, 3, mode));
        require(rowCount(reopened, "docs") == 8,
                "Cross-segment durable COMMIT did not survive reopen/REDO");
        reopened.catalog().validate();
    }
}

[[noreturn]] void loserChild(
    const std::string& path,
    std::uint32_t segmentBytes,
    minidb::WalUpdateMode mode) {
    ::setenv("MINIDB_FAILPOINT", "before_commit_append", 1);
    minidb::net::DatabaseServer server(path, config(segmentBytes, 2, mode));
    static_cast<void>(server.sqlEngine().execute(
        "INSERT INTO docs VALUES (99, '" + std::string(2500, 'z') + "')"));
    ::_exit(90);
}

void testCrossSegmentLoserUndo(minidb::WalUpdateMode mode) {
    minidb::test::TemporaryDatabase database("wal_cross_segment_loser");
    constexpr std::uint32_t SEGMENT_BYTES = 12U * 1024U;
    const auto path = database.path().string();
    {
        minidb::net::DatabaseServer server(path, config(SEGMENT_BYTES, 2, mode));
        static_cast<void>(server.sqlEngine().execute(
            "CREATE TABLE docs (id UINT32 PRIMARY KEY, body VARCHAR(3000) NOT NULL)"));
        static_cast<void>(server.checkpointManager().checkpoint());
    }
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) loserChild(path, SEGMENT_BYTES, mode);
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Cross-segment loser child missed pre-COMMIT failpoint");
    {
        minidb::net::DatabaseServer recovered(path, config(SEGMENT_BYTES, 3, mode));
        require(rowCount(recovered, "docs") == 0
                    && recovered.startupRecoveryStats().loserTransactions == 1,
                "Recovery did not UNDO the cross-segment loser");
        recovered.catalog().validate();
    }
}

[[noreturn]] void reclaimedHistoryLoserChild(
    const std::string& path,
    std::uint32_t segmentBytes,
    std::uint32_t key,
    minidb::WalUpdateMode mode) {
    ::setenv("MINIDB_FAILPOINT", "before_commit_append", 1);
    minidb::net::DatabaseServer server(path, config(segmentBytes, 1, mode));
    static_cast<void>(server.sqlEngine().execute(
        "INSERT INTO items VALUES (" + std::to_string(key) + ", 'loser')"));
    ::_exit(90);
}

void testManyCheckpointReclamationCyclesAndLostControl(minidb::WalUpdateMode mode) {
    minidb::test::TemporaryDatabase database("wal_many_reclaim_cycles");
    constexpr std::uint32_t SEGMENT_BYTES = 40U * 1024U;
    const auto path = database.path().string();
    std::uint32_t nextKey = 0;
    std::uint64_t logicalHighWater = 0;
    for (std::size_t cycle = 0; cycle < 8; ++cycle) {
        minidb::net::DatabaseServer server(path, config(SEGMENT_BYTES, 4, mode));
        if (cycle == 0) {
            static_cast<void>(server.sqlEngine().execute(
                "CREATE TABLE items (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
        }
        for (std::size_t row = 0; row < 12; ++row) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(nextKey++) + ", 'value')"));
        }
        static_cast<void>(server.checkpointManager().checkpoint());
        const auto stats = server.logManager().stats();
        require(stats.retainedSegments <= 5,
                "Periodic checkpointing did not bound retained segment count");
        require(stats.logicalWalEnd > logicalHighWater,
                "Checkpoint/reclamation cycle rebased the logical WAL high-water mark");
        logicalHighWater = stats.logicalWalEnd;
        server.catalog().validate();
    }

    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) reclaimedHistoryLoserChild(path, SEGMENT_BYTES, nextKey, mode);
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Post-reclamation loser child missed pre-COMMIT failpoint");

    std::filesystem::remove(minidb::checkpointPathForDatabase(path));
    {
        minidb::net::DatabaseServer recovered(path, config(SEGMENT_BYTES, 4, mode));
        require(recovered.startupRecoveryStats().checkpointUsed
                    && !recovered.startupRecoveryStats().fullScanFallback
                    && recovered.startupRecoveryStats().loserTransactions == 1
                    && rowCount(recovered, "items") == nextKey,
                "Retained-WAL checkpoint discovery did not undo the post-cycle loser");
        require(recovered.checkpointControl().select(recovered.logManager()).slot.has_value(),
                "Control loss fallback did not rebuild checkpoint metadata");
        recovered.catalog().validate();
        recovered.pageAllocator().validate();
    }
}

std::byte diskByte(
    minidb::DiskManager& disk,
    minidb::PageId pageId,
    std::size_t offset) {
    minidb::DiskManager::Page page{};
    disk.readPage(pageId, page);
    return page[offset];
}

void testMixedAdaptiveTransactionAcrossSegments(bool committed) {
    minidb::test::TemporaryDatabase database(committed
        ? "wal_adaptive_segment_winner" : "wal_adaptive_segment_loser");
    constexpr std::uint32_t SEGMENT_BYTES = 8'300;
    const auto path = database.path().string();
    const auto walPath = minidb::walPathForDatabase(path);
    minidb::PageId sparsePage = minidb::INVALID_PAGE_ID;
    minidb::PageId densePage = minidb::INVALID_PAGE_ID;
    minidb::TransactionId transactionId = minidb::INVALID_TRANSACTION_ID;
    {
        minidb::DiskManager disk(path);
        sparsePage = disk.appendPage();
        densePage = disk.appendPage();
        minidb::LogManager log(
            walPath, minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::EagerValidated,
            minidb::WalStorageMode::Segmented, SEGMENT_BYTES);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID,
            minidb::WalUpdateMode::Adaptive);
        minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        transactionId = coordinator.activeTransactionId();
        {
            auto page = pool.fetchPageWrite(sparsePage);
            page->data()[9] = std::byte{0x19};
        }
        {
            auto page = pool.fetchPageWrite(densePage);
            std::fill(page->data().begin(), page->data().end(), std::byte{0xE4});
        }
        require(pool.flushPage(densePage), "Could not flush dense segmented page");
        if (committed) coordinator.commitStatement();
        const auto stats = coordinator.stats();
        require(stats.adaptiveDeltaSelections == 1
                    && stats.adaptiveFullPageSelections == 1,
                "Segmented adaptive transaction did not select both encodings");
        if (pool.isResident(densePage)) pool.discardPageForRecovery(densePage);
    }

    minidb::SegmentedWalStorage storage(
        minidb::segmentedWalPathForDatabase(path), SEGMENT_BYTES, false);
    std::set<minidb::WalSegmentId> transactionSegments;
    bool sawFull = false;
    bool sawDelta = false;
    bool sawCommit = false;
    for (const auto& record : storage.scan().records) {
        if (record.transactionId != transactionId) continue;
        sawFull = sawFull || record.type == minidb::LogRecordType::PageUpdate;
        sawDelta = sawDelta || record.type == minidb::LogRecordType::PageDeltaUpdate;
        sawCommit = sawCommit || record.type == minidb::LogRecordType::Commit;
        for (const auto& segment : storage.segments()) {
            if (record.lsn >= segment.header.startLsn
                && record.lsn < segment.logicalEndLsn) {
                transactionSegments.insert(segment.header.segmentId);
                break;
            }
        }
    }
    require(sawFull && sawDelta && sawCommit == committed
                && transactionSegments.size() >= 2,
            "Mixed adaptive transaction did not span segments with a valid chain");

    {
        minidb::DiskManager disk(path);
        minidb::LogManager log(
            walPath, minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery,
            minidb::WalStorageMode::Auto, SEGMENT_BYTES);
        const auto recovery = minidb::RecoveryManager(disk, log).recover();
        const auto sparse = committed ? std::byte{0x19} : std::byte{0};
        const auto dense = committed ? std::byte{0xE4} : std::byte{0};
        require(diskByte(disk, sparsePage, 9) == sparse
                    && diskByte(disk, densePage, 0) == dense
                    && diskByte(disk, densePage, 4095) == dense,
                "Mixed adaptive cross-segment recovery produced the wrong state");
        require(committed ? recovery.pagesRedone == 2 : recovery.pagesUndone == 2,
                "Cross-segment adaptive recovery processed the wrong update count");
        log.validate();
    }
}

} // namespace

int main() {
    try {
        for (const auto mode : {minidb::WalUpdateMode::FullPage,
                                minidb::WalUpdateMode::ByteRange,
                                minidb::WalUpdateMode::Adaptive}) {
            testCrossSegmentCommitAndReopen(mode);
            testCrossSegmentLoserUndo(mode);
            testManyCheckpointReclamationCyclesAndLostControl(mode);
        }
        testMixedAdaptiveTransactionAcrossSegments(false);
        testMixedAdaptiveTransactionAcrossSegments(true);
        std::cout << "segmented_wal_recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "segmented_wal_recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
