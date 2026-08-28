#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/database_server.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/tuple_store.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace {

using minidb::test::require;

minidb::TupleBytes bytes(std::initializer_list<unsigned> values) {
    minidb::TupleBytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

minidb::Lsn lastPageLsnAwareUpdate(const minidb::LogManager& log) {
    minidb::Lsn result = minidb::INVALID_LSN;
    for (const auto& record : log.scan().records) {
        if (record.type == minidb::LogRecordType::PageUpdateV2
            || record.type == minidb::LogRecordType::PageDeltaUpdateV2) {
            result = record.lsn;
        }
    }
    return result;
}

void writeLegacyFormatHeader(const std::filesystem::path& path) {
    minidb::DiskManager disk(path.string());
    minidb::DiskManager::Page page{};
    disk.readPhysicalPage(minidb::database_format::METADATA_PAGE_ID, page);
    minidb::byte_codec::writeUint32(
        page, minidb::database_format::FORMAT_VERSION_OFFSET,
        minidb::database_format::LEGACY_VERSION);
    minidb::clearPersistentPageLsn(page);
    disk.writePhysicalPage(minidb::database_format::METADATA_PAGE_ID, page);
    disk.sync();
}

void testV1StartupMigrationAndIdempotentReopen() {
    minidb::test::TemporaryDatabase database("page_lsn_v1_migration");
    writeLegacyFormatHeader(database.path());
    {
        minidb::net::DatabaseServer server(
            database.path().string(),
            minidb::net::ServerConfig{"127.0.0.1", 0, 8, 32, 2});
        require(server.diskManager().databaseHeader().formatVersion
                    == minidb::database_format::CURRENT_VERSION,
                "Legacy database did not migrate to format v2");
        minidb::DiskManager::Page metadata{};
        server.diskManager().readPhysicalPage(0, metadata);
        require(minidb::readPersistentPageLsn(metadata) != minidb::INVALID_LSN,
                "Format transition did not persist page 0 PageLSN");
        server.catalog().validate();
    }
    {
        minidb::net::DatabaseServer reopened(
            database.path().string(),
            minidb::net::ServerConfig{"127.0.0.1", 0, 8, 32, 2});
        require(reopened.diskManager().databaseHeader().formatVersion
                    == minidb::database_format::CURRENT_VERSION,
                "Migrated database did not remain at format v2");
        reopened.catalog().validate();
    }
}

void runSelectiveRedoCase(minidb::WalUpdateMode mode, bool persistUpdate) {
    minidb::test::TemporaryDatabase database(
        persistUpdate ? "page_lsn_selective_persisted" : "page_lsn_selective_noforce");
    minidb::PageId heapMetadataPageId = minidb::INVALID_PAGE_ID;
    minidb::RecordId rid{};
    minidb::Lsn updateLsn = minidb::INVALID_LSN;
    minidb::LogRecordType updateType = minidb::LogRecordType::Begin;
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        const auto startup = minidb::RecoveryManager(disk, log, &control).recover();
        minidb::RecoveryCoordinator coordinator(
            disk, log, startup.nextTransactionId, mode);
        minidb::BufferPoolManager pool(disk, 16, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        minidb::DatabaseMetadataManager metadata(disk, coordinator, log);
        minidb::PageAllocator allocator(pool, disk, &metadata);
        minidb::CheckpointManager checkpoints(
            coordinator, pool, disk, log, control, startup);

        coordinator.beginStatement();
        auto store = minidb::TupleStore::create(pool, disk, allocator);
        heapMetadataPageId = store.metadataPageId();
        rid = store.insert(bytes({1, 2, 3}));
        coordinator.commitStatement();
        static_cast<void>(checkpoints.checkpoint());

        coordinator.beginStatement();
        require(store.tryUpdate(rid, bytes({9, 8, 7, 6})),
                "Controlled existing-page update unexpectedly lacked space");
        coordinator.commitStatement();
        for (const auto& record : log.scan().records) {
            if (record.type == minidb::LogRecordType::PageUpdateV2
                || record.type == minidb::LogRecordType::PageDeltaUpdateV2) {
                updateLsn = record.lsn;
                updateType = record.type;
            }
        }
        require(minidb::isValidLsn(updateLsn),
                "Format-v2 transaction did not generate a PageLSN-aware record");
        if (mode == minidb::WalUpdateMode::FullPage) {
            require(updateType == minidb::LogRecordType::PageUpdateV2,
                    "FullPage mode did not generate PAGE_UPDATE_V2");
        } else if (mode == minidb::WalUpdateMode::ByteRange) {
            require(updateType == minidb::LogRecordType::PageDeltaUpdateV2,
                    "ByteRange mode did not generate PAGE_DELTA_UPDATE_V2");
        }

        if (persistUpdate) {
            pool.flushAll();
            minidb::DiskManager::Page page{};
            disk.readPhysicalPage(rid.pageId, page);
            require(minidb::readPersistentPageLsn(page) == updateLsn,
                    "Normal flush did not persist the update record LSN in-page");
        } else {
            pool.discardPageForRecovery(rid.pageId);
        }
    }

    minidb::RecoveryStats selective;
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(
            minidb::walPathForDatabase(database.path().string()),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        selective = minidb::RecoveryManager(
            disk, log, &control, false,
            minidb::RedoPolicy::PageLsnSelectiveRedo).recover();
        minidb::DiskManager::Page page{};
        disk.readPhysicalPage(rid.pageId, page);
        require(minidb::readPersistentPageLsn(page) == updateLsn,
                "Selective REDO did not leave the exact update PageLSN");
    }
    require(selective.pageLsnChecks == 1,
            "Selective REDO did not perform exactly one existing-page PageLSN check");
    if (persistUpdate) {
        require(selective.redoSkippedByPageLsn == 1
                    && selective.pagesRedone == 0
                    && selective.recoveryPageWrites == 0,
                "Selective REDO did not skip the already-persisted update");
    } else {
        require(selective.redoSkippedByPageLsn == 0
                    && selective.redoAppliedAfterPageLsnCheck == 1
                    && selective.pagesRedone == 1,
                "Selective REDO did not apply the missing NO-FORCE update");
    }

    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(
            minidb::walPathForDatabase(database.path().string()),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        const auto always = minidb::RecoveryManager(
            disk, log, &control, false, minidb::RedoPolicy::AlwaysRedo).recover();
        require(always.pagesRedone == 1 && always.recoveryPageWrites == 1
                    && always.redoSkippedByPageLsn == 0,
                "AlwaysRedo control did not replay the PageLSN-aware update");

        minidb::BufferPoolManager pool(disk, 8);
        minidb::PageAllocator allocator(pool, disk);
        auto store = minidb::TupleStore::open(
            pool, disk, allocator, heapMetadataPageId);
        require(store.get(rid) == bytes({9, 8, 7, 6}),
                "AlwaysRedo and selective recovery produced different tuple bytes");
        store.validate();
    }
}

void testSelectiveRedoAcrossAllGenerationModes() {
    for (const auto mode : {
             minidb::WalUpdateMode::FullPage,
             minidb::WalUpdateMode::ByteRange,
             minidb::WalUpdateMode::Adaptive,
         }) {
        runSelectiveRedoCase(mode, true);
        runSelectiveRedoCase(mode, false);
    }
}

void testWinnerRedoSkipThenLoserUndoRestoresBeforePageLsn() {
    minidb::test::TemporaryDatabase database("page_lsn_winner_loser_same_page");
    minidb::PageId heapMetadataPageId = minidb::INVALID_PAGE_ID;
    minidb::RecordId rid{};
    minidb::Lsn winnerLsn = minidb::INVALID_LSN;
    minidb::Lsn loserLsn = minidb::INVALID_LSN;
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        const auto startup = minidb::RecoveryManager(disk, log, &control).recover();
        minidb::RecoveryCoordinator coordinator(
            disk, log, startup.nextTransactionId, minidb::WalUpdateMode::Adaptive);
        minidb::BufferPoolManager pool(disk, 8, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        minidb::DatabaseMetadataManager metadata(disk, coordinator, log);
        minidb::PageAllocator allocator(pool, disk, &metadata);
        minidb::CheckpointManager checkpoints(
            coordinator, pool, disk, log, control, startup);
        coordinator.beginStatement();
        auto store = minidb::TupleStore::create(pool, disk, allocator);
        heapMetadataPageId = store.metadataPageId();
        rid = store.insert(bytes({1, 1, 1}));
        coordinator.commitStatement();
        static_cast<void>(checkpoints.checkpoint());

        coordinator.beginStatement();
        require(store.tryUpdate(rid, bytes({2, 2, 2})), "Winner update failed");
        coordinator.commitStatement();
        winnerLsn = lastPageLsnAwareUpdate(log);

        coordinator.beginStatement();
        require(store.tryUpdate(rid, bytes({3, 3, 3})), "Loser update failed");
        pool.flushAll();
        loserLsn = lastPageLsnAwareUpdate(log);
        minidb::DiskManager::Page stolen{};
        disk.readPhysicalPage(rid.pageId, stolen);
        require(minidb::readPersistentPageLsn(stolen) == loserLsn
                    && loserLsn > winnerLsn,
                "STEAL setup did not persist the loser PageLSN");
    }
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(
            minidb::walPathForDatabase(database.path().string()),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        const auto recovered = minidb::RecoveryManager(disk, log, &control).recover();
        require(recovered.redoSkippedByPageLsn == 1 && recovered.pagesUndone == 1,
                "Winner/loser recovery did not skip winner then undo loser");
        minidb::DiskManager::Page page{};
        disk.readPhysicalPage(rid.pageId, page);
        require(minidb::readPersistentPageLsn(page) == winnerLsn,
                "Loser UNDO did not restore the winner-visible beforePageLsn");
        minidb::BufferPoolManager pool(disk, 8);
        minidb::PageAllocator allocator(pool, disk);
        auto store = minidb::TupleStore::open(pool, disk, allocator, heapMetadataPageId);
        require(store.get(rid) == bytes({2, 2, 2}),
                "Loser UNDO did not restore the winner tuple bytes");
    }
}

void testMultipleRecordsOnOnePageSkipSkipApply() {
    minidb::test::TemporaryDatabase database("page_lsn_r1_r2_r3");
    minidb::PageId heapMetadataPageId = minidb::INVALID_PAGE_ID;
    minidb::RecordId rid{};
    std::array<minidb::Lsn, 3> lsns{};
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        const auto startup = minidb::RecoveryManager(disk, log, &control).recover();
        minidb::RecoveryCoordinator coordinator(
            disk, log, startup.nextTransactionId, minidb::WalUpdateMode::ByteRange);
        minidb::BufferPoolManager pool(disk, 8, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        minidb::DatabaseMetadataManager metadata(disk, coordinator, log);
        minidb::PageAllocator allocator(pool, disk, &metadata);
        minidb::CheckpointManager checkpoints(
            coordinator, pool, disk, log, control, startup);
        coordinator.beginStatement();
        auto store = minidb::TupleStore::create(pool, disk, allocator);
        heapMetadataPageId = store.metadataPageId();
        rid = store.insert(bytes({0}));
        coordinator.commitStatement();
        static_cast<void>(checkpoints.checkpoint());

        for (std::size_t index = 0; index < lsns.size(); ++index) {
            coordinator.beginStatement();
            require(store.tryUpdate(rid, bytes({static_cast<unsigned>(index + 1)})),
                    "Repeated same-page update failed");
            coordinator.commitStatement();
            lsns[index] = lastPageLsnAwareUpdate(log);
            if (index == 1) pool.flushAll();
        }
        pool.discardPageForRecovery(rid.pageId);
    }
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(
            minidb::walPathForDatabase(database.path().string()),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        minidb::CheckpointControl control(
            minidb::checkpointPathForDatabase(database.path().string()));
        const auto recovered = minidb::RecoveryManager(disk, log, &control).recover();
        require(recovered.pageLsnChecks == 3
                    && recovered.redoSkippedByPageLsn == 2
                    && recovered.redoAppliedAfterPageLsnCheck == 1,
                "R1/R2/R3 recovery did not perform skip/skip/apply");
        minidb::DiskManager::Page page{};
        disk.readPhysicalPage(rid.pageId, page);
        require(minidb::readPersistentPageLsn(page) == lsns[2],
                "R3 REDO did not install its PageLSN");
        minidb::BufferPoolManager pool(disk, 8);
        minidb::PageAllocator allocator(pool, disk);
        auto store = minidb::TupleStore::open(pool, disk, allocator, heapMetadataPageId);
        require(store.get(rid) == bytes({3}), "R3 tuple state was not recovered");
    }
}

} // namespace

int main() {
    try {
        testV1StartupMigrationAndIdempotentReopen();
        testSelectiveRedoAcrossAllGenerationModes();
        testWinnerRedoSkipThenLoserUndoRestoresBeforePageLsn();
        testMultipleRecordsOnOnePageSkipSkipApply();
        std::cout << "persistent PageLSN recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent PageLSN recovery test failure: " << error.what() << '\n';
        return 1;
    }
}
