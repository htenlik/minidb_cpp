#include "minidb/database_server.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {
using minidb::test::require;

minidb::net::ServerConfig config() {
    minidb::net::ServerConfig result;
    result.port = 0;
    result.bufferFrames = 16;
    result.checkpointWalBytes = 0;
    result.checkpointMode = minidb::CheckpointMode::Fuzzy;
    result.walSegmentBytes = 32 * 1024;
    return result;
}

minidb::CheckpointId initialize(const std::string& path) {
    minidb::net::DatabaseServer server(path, config());
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE events (id UINT32 PRIMARY KEY, value VARCHAR(1000) NOT NULL)"));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO events VALUES (1, 'base')"));
    return server.checkpointManager().checkpoint(minidb::CheckpointMode::Sharp);
}

std::size_t rowCount(minidb::net::DatabaseServer& server) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM events")).rows.size();
}

[[noreturn]] void childCheckpoint(const std::string& path, const char* failpoint) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::net::DatabaseServer server(path, config());
    static_cast<void>(server.sqlEngine().execute("INSERT INTO events VALUES (2, 'tail')"));
    static_cast<void>(server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy));
    ::_exit(90);
}

void expectFailpointCrash(const std::string& path, const char* failpoint) {
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) childCheckpoint(path, failpoint);
    int status = 0;
    if (::waitpid(child, &status, 0) != child) throw std::runtime_error("waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            std::string("Child did not hit fuzzy failpoint ") + failpoint);
}

void testCrashPublicationMatrix() {
    for (const auto* failpoint : {
             "fuzzy_checkpoint_after_begin_append",
             "fuzzy_checkpoint_after_dpt_snapshot",
             "fuzzy_checkpoint_after_end_append",
             "fuzzy_checkpoint_after_end_fsync",
             "checkpoint_mid_control_write",
             "fuzzy_checkpoint_after_control_sync",
             "fuzzy_checkpoint_during_reclamation",
         }) {
        minidb::test::TemporaryDatabase database(
            std::string("fuzzy_crash_") + failpoint);
        const auto sharpId = initialize(database.path().string());
        expectFailpointCrash(database.path().string(), failpoint);
        minidb::net::DatabaseServer server(database.path().string(), config());
        const bool published = std::string_view(failpoint)
                == "fuzzy_checkpoint_after_control_sync"
            || std::string_view(failpoint) == "fuzzy_checkpoint_during_reclamation";
        require(server.startupRecoveryStats().checkpointUsed
                    && server.startupRecoveryStats().checkpointId
                        == sharpId + (published ? 1 : 0)
                    && rowCount(server) == 2,
                std::string("Fuzzy checkpoint crash boundary failed at ") + failpoint);
        server.catalog().validate();
        server.pageAllocator().validate();
    }
}

void testLostControlDiscovery() {
    minidb::test::TemporaryDatabase database("fuzzy_lost_control");
    minidb::CheckpointId fuzzyId = minidb::INVALID_CHECKPOINT_ID;
    {
        minidb::net::DatabaseServer server(database.path().string(), config());
        static_cast<void>(server.sqlEngine().execute(
            "CREATE TABLE events (id UINT32 PRIMARY KEY, value VARCHAR(1000) NOT NULL)"));
        for (std::uint32_t key = 0; key < 25; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO events VALUES (" + std::to_string(key) + ", 'v')"));
        }
        fuzzyId = server.checkpointManager().checkpoint(minidb::CheckpointMode::Fuzzy);
    }
    std::filesystem::remove(database.path().string() + ".ckpt");
    minidb::net::DatabaseServer server(database.path().string(), config());
    require(server.startupRecoveryStats().checkpointUsed
                && server.startupRecoveryStats().checkpointId == fuzzyId
                && server.startupRecoveryStats().checkpointMode
                    == minidb::CheckpointMode::Fuzzy
                && rowCount(server) == 25,
            "Retained WAL did not reconstruct lost fuzzy checkpoint control");
    require(server.checkpointControl().select(server.logManager()).slot.has_value(),
            "Lost fuzzy checkpoint control was not rebuilt");
}
} // namespace

int main() {
    try {
        testCrashPublicationMatrix();
        testLostControlDiscovery();
        std::cout << "fuzzy_checkpoint_crash_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fuzzy_checkpoint_crash_test failed: " << error.what() << '\n';
        return 1;
    }
}
