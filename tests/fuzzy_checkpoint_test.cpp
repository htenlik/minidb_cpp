#include "minidb/checkpoint_log.hpp"
#include "minidb/database_server.hpp"
#include "minidb/page_access.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

namespace {
using minidb::test::require;

minidb::net::ServerConfig fuzzyConfig(
    std::uint64_t statements = 0,
    std::uint32_t segmentBytes = minidb::wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY,
    minidb::WalUpdateMode walMode = minidb::WalUpdateMode::FullPage) {
    minidb::net::ServerConfig config;
    config.port = 0;
    config.bufferFrames = 32;
    config.checkpointWalBytes = 0;
    config.checkpointStatements = statements;
    config.walSegmentBytes = segmentBytes;
    config.walUpdateMode = walMode;
    config.checkpointMode = minidb::CheckpointMode::Fuzzy;
    return config;
}

void createItems(minidb::net::DatabaseServer& server) {
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE items (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
}

std::size_t rowCount(minidb::net::DatabaseServer& server) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM items")).rows.size();
}

void testFuzzyCheckpointDoesNotFlushAndAllowsPins() {
    minidb::test::TemporaryDatabase database("fuzzy_no_flush");
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
    createItems(server);
    static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (1, 'alpha')"));

    const auto beforeDpt = server.bufferPool().dirtyPageTableSnapshot();
    require(!beforeDpt.empty(), "Committed mutation did not populate the DPT");
    for (const auto& entry : beforeDpt) {
        require(minidb::isValidLsn(entry.recLsn)
                    && minidb::isValidLsn(entry.pageLsn)
                    && entry.recLsn <= entry.pageLsn,
                "DPT entry has an invalid recLSN/PageLSN relationship");
    }
    const auto writesBefore = server.bufferPool().stats().physicalPageWrites;
    auto pinned = minidb::requireReadPage(
        server.bufferPool(), beforeDpt.front().pageId, "pin dirty fuzzy-checkpoint page");
    const auto id = server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy);
    require(id != minidb::INVALID_CHECKPOINT_ID,
            "Fuzzy checkpoint did not return a valid identity");
    require(server.bufferPool().stats().physicalPageWrites == writesBefore,
            "Fuzzy checkpoint flushed a database page");
    require(server.bufferPool().dirtyPageTableSnapshot() == beforeDpt,
            "Fuzzy checkpoint changed the live DPT");
    const auto stats = server.checkpointManager().stats();
    require(stats.fuzzyCheckpointsCompleted == 1
                && stats.dptEntriesCaptured == beforeDpt.size()
                && stats.databaseSyncs == 1 // the preceding sharp checkpoint only
                && stats.pinnedFramesObserved >= 1,
            "Fuzzy checkpoint statistics are incorrect");
    pinned.drop();

    const auto selection = server.checkpointControl().select(server.logManager());
    require(selection.slot.has_value() && selection.slot->checkpointId == id,
            "Checkpoint control did not select the fuzzy checkpoint");
    const auto end = server.logManager().readRecordAt(selection.slot->checkpointEndLsn);
    require(end.type == minidb::LogRecordType::FuzzyCheckpointEnd,
            "Control slot did not reference FUZZY_CHECKPOINT_END");
    const auto payload = minidb::decodeFuzzyCheckpointEndLogPayload(end.payload);
    require(payload.dirtyPages.size() == beforeDpt.size()
                && payload.activeTransactions.empty(),
            "Persisted fuzzy checkpoint snapshot is incorrect");
}

void testFuzzyReopenAndContinuedTail() {
    minidb::test::TemporaryDatabase database("fuzzy_reopen");
    minidb::CheckpointId checkpointId = minidb::INVALID_CHECKPOINT_ID;
    {
        minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
        createItems(server);
        for (std::uint32_t key = 0; key < 80; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(key) + ", 'before')"));
        }
        checkpointId = server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy);
        for (std::uint32_t key = 80; key < 100; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(key) + ", 'tail')"));
        }
    }
    {
        minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
        const auto& recovery = server.startupRecoveryStats();
        require(recovery.checkpointUsed
                    && recovery.checkpointMode == minidb::CheckpointMode::Fuzzy
                    && recovery.checkpointId == checkpointId
                    && recovery.checkpointDirtyPageCount != 0
                    && minidb::isValidLsn(recovery.redoStartLsn),
                "Recovery did not initialize from the fuzzy checkpoint DPT");
        require(recovery.redoCandidates
                    == recovery.redoApplied + recovery.redoSkippedNotInDpt
                        + recovery.redoSkippedBeforeRecLsn
                        + recovery.redoSkippedByPageLsn,
                "DPT/PageLSN REDO filter counters do not compose exactly");
        require(rowCount(server) == 100, "Fuzzy checkpoint recovery lost rows");
        server.catalog().validate();
        server.pageAllocator().validate();
        static_cast<void>(server.sqlEngine().execute(
            "UPDATE items SET value = 'after-reopen' WHERE id = 50"));
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
    }
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
    require(rowCount(server) == 100, "Repeated fuzzy reopen lost rows");
}

void testEmptyDptAndMixedModes() {
    minidb::test::TemporaryDatabase database("fuzzy_mixed_modes");
    {
        minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp));
        require(server.bufferPool().dirtyPageTableSnapshot().empty(),
                "Sharp checkpoint did not empty the DPT");
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
        require(server.checkpointManager().stats().oldestRecLsn == minidb::INVALID_LSN,
                "Empty-DPT fuzzy checkpoint reported an oldest recLSN");
        createItems(server);
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp));
        require(server.bufferPool().dirtyPageTableSnapshot().empty(),
                "Final sharp checkpoint did not empty a fuzzy-era DPT");
    }
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
    require(server.startupRecoveryStats().checkpointUsed
                && server.startupRecoveryStats().checkpointMode
                    == minidb::CheckpointMode::Sharp,
            "Sharp-to-fuzzy-to-sharp sequence did not reopen from the newest checkpoint");
    server.catalog().validate();
}

void testAutomaticFuzzyPolicy() {
    minidb::test::TemporaryDatabase database("fuzzy_auto");
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig(1));
    createItems(server);
    require(server.checkpointManager().stats().fuzzyCheckpointsCompleted == 1,
            "Automatic statement checkpoint did not use configured fuzzy mode");
    require(!server.bufferPool().dirtyPageTableSnapshot().empty(),
            "Automatic fuzzy checkpoint unexpectedly flushed dirty pages");
}

void testDptEvolutionAndManyCheckpoints() {
    minidb::test::TemporaryDatabase database("fuzzy_dpt_evolution");
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
    createItems(server);
    static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (1, 'one')"));
    const auto firstPeriod = server.bufferPool().dirtyPageTableSnapshot();
    require(!firstPeriod.empty(), "DPT evolution setup has no dirty pages");
    const auto oldMaximum = std::max_element(
        firstPeriod.begin(), firstPeriod.end(),
        [](const auto& left, const auto& right) { return left.recLsn < right.recLsn; })
        ->recLsn;
    auto previousId = server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy);
    for (const auto& entry : firstPeriod) {
        require(server.bufferPool().flushPage(entry.pageId),
                "Could not flush a captured DPT page");
    }
    require(server.bufferPool().dirtyPageTableSnapshot().empty(),
            "Successful page flushes did not remove DPT entries");
    const auto emptyId = server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy);
    require(emptyId > previousId, "Fuzzy CheckpointId did not remain monotonic");
    previousId = emptyId;

    static_cast<void>(server.sqlEngine().execute(
        "UPDATE items SET value = 'new-dirty-period' WHERE id = 1"));
    const auto secondPeriod = server.bufferPool().dirtyPageTableSnapshot();
    require(!secondPeriod.empty()
                && std::all_of(secondPeriod.begin(), secondPeriod.end(),
                    [oldMaximum](const auto& entry) { return entry.recLsn > oldMaximum; }),
            "A new dirty period reused an old recLSN");
    for (std::size_t index = 0; index < 8; ++index) {
        const auto id = server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy);
        require(id > previousId, "Repeated fuzzy checkpoint ID did not increase");
        previousId = id;
    }
    require(server.bufferPool().dirtyPageTableSnapshot() == secondPeriod,
            "Repeated fuzzy checkpoints changed dirty-page state");
}

void testReverseMixedModes() {
    minidb::test::TemporaryDatabase database("fuzzy_reverse_modes");
    {
        minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
        createItems(server);
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
        static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (1, 'v')"));
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp));
        static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (2, 'v')"));
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
    }
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig());
    require(server.startupRecoveryStats().checkpointMode == minidb::CheckpointMode::Fuzzy
                && rowCount(server) == 2,
            "Fuzzy-to-sharp-to-fuzzy sequence did not recover correctly");
}

void testAllWalUpdateModes() {
    for (const auto mode : {
             minidb::WalUpdateMode::FullPage,
             minidb::WalUpdateMode::ByteRange,
             minidb::WalUpdateMode::Adaptive,
         }) {
        minidb::test::TemporaryDatabase database(
            "fuzzy_wal_mode_" + std::string(minidb::walUpdateModeName(mode)));
        {
            minidb::net::DatabaseServer server(
                database.path().string(), fuzzyConfig(0,
                    minidb::wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY, mode));
            createItems(server);
            for (std::uint32_t key = 0; key < 12; ++key) {
                static_cast<void>(server.sqlEngine().execute(
                    "INSERT INTO items VALUES (" + std::to_string(key) + ", 'mode')"));
            }
            static_cast<void>(server.checkpointManager().checkpoint(
                minidb::CheckpointMode::Fuzzy));
        }
        minidb::net::DatabaseServer server(
            database.path().string(), fuzzyConfig(0,
                minidb::wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY, mode));
        require(server.startupRecoveryStats().checkpointMode
                    == minidb::CheckpointMode::Fuzzy
                    && rowCount(server) == 12,
                "Fuzzy recovery failed for WAL update mode "
                    + std::string(minidb::walUpdateModeName(mode)));
    }
}

void testLongLivedDirtyPageRetentionFloor() {
    minidb::test::TemporaryDatabase database("fuzzy_long_dirty_retention");
    minidb::net::DatabaseServer server(database.path().string(), fuzzyConfig(0, 16 * 1024));
    createItems(server);
    static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (1, 'anchor')"));
    const auto initialDpt = server.bufferPool().dirtyPageTableSnapshot();
    require(!initialDpt.empty(), "Long-lived dirty-page setup has an empty DPT");
    const auto oldestRec = std::min_element(
        initialDpt.begin(), initialDpt.end(),
        [](const auto& left, const auto& right) { return left.recLsn < right.recLsn; })
        ->recLsn;

    for (std::uint32_t key = 2; key < 10; ++key) {
        static_cast<void>(server.sqlEngine().execute(
            "INSERT INTO items VALUES (" + std::to_string(key) + ", 'tail')"));
        static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
        require(server.checkpointManager().stats().retentionFloorLsn == oldestRec,
                "Long-lived dirty page did not pin the fuzzy retention floor");
    }
    require(server.logManager().oldestRetainedLsn() <= oldestRec,
            "Required recLSN history was reclaimed while its page remained dirty");

    const auto dirty = server.bufferPool().dirtyPageTableSnapshot();
    for (const auto& entry : dirty) {
        require(server.bufferPool().flushPage(entry.pageId),
                "Could not flush long-lived dirty page");
    }
    static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
    require(server.checkpointManager().stats().retentionFloorLsn > oldestRec
                && server.logManager().oldestRetainedLsn() > oldestRec,
            "Flushing the long-lived page did not advance recovery/reclamation history");
}
} // namespace

int main() {
    try {
        testFuzzyCheckpointDoesNotFlushAndAllowsPins();
        testFuzzyReopenAndContinuedTail();
        testEmptyDptAndMixedModes();
        testAutomaticFuzzyPolicy();
        testDptEvolutionAndManyCheckpoints();
        testReverseMixedModes();
        testAllWalUpdateModes();
        testLongLivedDirtyPageRetentionFloor();
        std::cout << "fuzzy_checkpoint_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fuzzy_checkpoint_test failed: " << error.what() << '\n';
        return 1;
    }
}
