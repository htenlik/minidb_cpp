#include "minidb/checkpoint_control.hpp"
#include "minidb/database_server.hpp"
#include "minidb/page_access.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <variant>

namespace {
using minidb::test::require;

minidb::net::ServerConfig config(std::uint64_t walBytes = 0, std::uint64_t statements = 0) {
    return {"127.0.0.1", 0, 8, 8, 2, walBytes, statements};
}

std::size_t rows(minidb::net::DatabaseServer& server) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM items")).rows.size();
}

void createItems(minidb::net::DatabaseServer& server) {
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE items (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
}

void testBasicCheckpointAndTailRecovery() {
    minidb::test::TemporaryDatabase database("checkpoint_basic");
    minidb::CheckpointId first = 0;
    minidb::PageId catalogRoot = minidb::INVALID_PAGE_ID;
    minidb::TransactionId nextAtCheckpoint = 0;
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        createItems(server);
        for (std::uint32_t key = 0; key < 40; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(key) + ", 'v')"));
        }
        require(server.bufferPool().stats().pinnedFrames == 0,
                "Completed statements retained pins before checkpoint");
        first = server.checkpointManager().checkpoint();
        catalogRoot = server.diskManager().databaseHeader().catalogRootPageId;
        nextAtCheckpoint = server.recoveryCoordinator().nextTransactionId();
        const auto stats = server.checkpointManager().stats();
        require(first == 1 && stats.checkpointsCompleted == 1
                    && stats.databaseSyncs == 1 && stats.walForces == 1
                    && stats.lastRecoveryStartOffset > stats.lastCheckpointEndLsn,
                "Basic checkpoint statistics/identity are incorrect");
        require(server.bufferPool().stats().pinnedFrames == 0
                    && server.bufferPool().stats().physicalPageWrites >= stats.dirtyPagesFlushed,
                "Checkpoint did not leave a quiescent buffer pool");
        for (std::uint32_t key = 40; key < 50; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(key) + ", 'tail')"));
        }
    }
    require(std::filesystem::file_size(database.path().string() + ".ckpt") == 192,
            "Checkpoint control file does not have fixed size 192");
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        const auto& recovery = server.startupRecoveryStats();
        require(recovery.checkpointUsed && !recovery.fullScanFallback
                    && recovery.checkpointId == first
                    && recovery.recoveryStartOffset > minidb::wal_file_layout::HEADER_SIZE
                    && recovery.walBytesSkipped
                        == recovery.recoveryStartOffset - minidb::wal_file_layout::HEADER_SIZE
                    && recovery.recordsAnalyzed < 100,
                "Restart did not bound recovery to the post-checkpoint tail");
        require(server.recoveryCoordinator().nextTransactionId() >= nextAtCheckpoint,
                "Checkpoint restart regressed TransactionId");
        require(rows(server) == 50
                    && server.diskManager().databaseHeader().catalogRootPageId == catalogRoot,
                "Checkpoint/tail recovery lost catalog or row state");
        server.catalog().validate();
        server.pageAllocator().validate();
        const auto second = server.checkpointManager().checkpoint();
        require(second > first, "CheckpointId did not increase after reopen");
    }
}

void testNoDirtyQuiescenceAndAutoPolicy() {
    minidb::test::TemporaryDatabase database("checkpoint_policy");
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        static_cast<void>(server.checkpointManager().checkpoint());
        server.bufferPool().resetStats();
        server.checkpointManager().resetStats();
        const auto writes = server.bufferPool().stats().physicalPageWrites;
        static_cast<void>(server.checkpointManager().checkpoint());
        const auto after = server.checkpointManager().stats();
        require(after.dirtyPagesFlushed == 0
                    && server.bufferPool().stats().physicalPageWrites == writes,
                "Clean checkpoint performed database page writes");

        server.recoveryCoordinator().beginStatement();
        minidb::test::requireThrows<std::logic_error>(
            [&] { static_cast<void>(server.checkpointManager().checkpoint()); },
            "Checkpoint ran during an active transaction");
        server.recoveryCoordinator().commitStatement();

        auto guard = minidb::requireReadPage(
            server.bufferPool(), server.catalog().metadataPageId(), "pin checkpoint test page");
        minidb::test::requireThrows<std::logic_error>(
            [&] { static_cast<void>(server.checkpointManager().checkpoint()); },
            "Checkpoint ran with a pinned page");
        guard.drop();
    }

    minidb::test::TemporaryDatabase automatic("checkpoint_auto");
    {
        minidb::net::DatabaseServer server(automatic.path().string(), config(0, 2));
        createItems(server);
        require(server.checkpointManager().stats().checkpointsCompleted == 0,
                "Statement-count checkpoint fired too early");
        static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (1, 'v')"));
        require(server.checkpointManager().stats().checkpointsCompleted == 1
                    && !server.recoveryCoordinator().hasActiveStatement(),
                "Automatic checkpoint did not run after committed statement threshold");
    }

    minidb::test::TemporaryDatabase disabled("checkpoint_disabled");
    {
        minidb::net::DatabaseServer server(disabled.path().string(), config());
        createItems(server);
        for (std::uint32_t key = 0; key < 5; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(key) + ", 'v')"));
        }
        require(server.checkpointManager().stats().checkpointsCompleted == 0,
                "Disabled automatic checkpoint policy fired");
        static_cast<void>(server.checkpointManager().checkpoint());
        require(server.checkpointManager().stats().checkpointsCompleted == 1,
                "Manual checkpoint was disabled with automatic policy");
    }
}

void corruptByte(const std::string& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    char value = 0;
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(&value, 1);
    value ^= 1;
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1);
    file.flush();
    if (!file) throw std::runtime_error("Could not corrupt checkpoint-control byte");
}

void testTornNewestAndFullScanFallback() {
    minidb::test::TemporaryDatabase database("checkpoint_torn_control");
    minidb::CheckpointId older = 0;
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        createItems(server);
        older = server.checkpointManager().checkpoint();
        static_cast<void>(server.sqlEngine().execute("INSERT INTO items VALUES (1, 'v')"));
        static_cast<void>(server.checkpointManager().checkpoint());
    }
    const auto controlPath = database.path().string() + ".ckpt";
    // Generation 1 occupies slot 0 and generation 2 occupies slot 1.
    corruptByte(controlPath, 64 + 64 + 12);
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        const auto& stats = server.startupRecoveryStats();
        require(stats.checkpointUsed && stats.checkpointId == older
                    && stats.checkpointValidationFailures >= 1 && rows(server) == 1,
                "Torn newest slot did not fall back to the older valid generation");
        require(server.checkpointManager().nextCheckpointId() >= 3,
                "Torn-slot fallback would reuse a CheckpointId");
    }

    corruptByte(controlPath, 64 + 7);
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        const auto& stats = server.startupRecoveryStats();
        require(!stats.checkpointUsed && stats.fullScanFallback
                    && stats.checkpointControlPresent
                    && stats.checkpointValidationFailures >= 2 && rows(server) == 1,
                "Two invalid control slots did not trigger correct full-scan fallback");
    }
}

void testFailureDoesNotPublish() {
    minidb::test::TemporaryDatabase database("checkpoint_failure");
    minidb::net::DatabaseServer server(database.path().string(), config());
    createItems(server);
    const auto original = server.checkpointManager().checkpoint();
    for (const auto* point : {"checkpoint_after_buffer_flush",
                              "checkpoint_after_database_sync",
                              "checkpoint_after_end_fsync"}) {
        ::setenv("MINIDB_THROWPOINT", point, 1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(server.checkpointManager().checkpoint()); },
            "Injected checkpoint failure did not surface");
        ::unsetenv("MINIDB_THROWPOINT");
        const auto selected = server.checkpointControl().select(server.logManager());
        require(selected.slot.has_value() && selected.slot->checkpointId == original,
                "Failed checkpoint published a new authoritative slot");
    }
    require(server.checkpointManager().stats().checkpointFailures == 3,
            "Checkpoint failure statistics are incorrect");
}
} // namespace

int main() {
    try {
        testBasicCheckpointAndTailRecovery();
        testNoDirtyQuiescenceAndAutoPolicy();
        testTornNewestAndFullScanFallback();
        testFailureDoesNotPublish();
        std::cout << "checkpoint_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        ::unsetenv("MINIDB_THROWPOINT");
        std::cerr << "checkpoint_test failed: " << error.what() << '\n';
        return 1;
    }
}
