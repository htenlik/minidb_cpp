#include "minidb/database_server.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {
using minidb::test::require;

minidb::net::ServerConfig config(std::size_t frames = 4) {
    return {"127.0.0.1", 0, 8, frames, 2, 0, 0};
}

minidb::CheckpointId initialize(const std::string& path) {
    minidb::net::DatabaseServer server(path, config());
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE events (id UINT32 PRIMARY KEY, value VARCHAR(3000) NOT NULL)"));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO events VALUES (1, 'base')"));
    return server.checkpointManager().checkpoint();
}

[[noreturn]] void checkpointChild(const std::string& path, const char* failpoint) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::net::DatabaseServer server(path, config());
    static_cast<void>(server.sqlEngine().execute("INSERT INTO events VALUES (2, 'tail')"));
    static_cast<void>(server.checkpointManager().checkpoint());
    ::_exit(90);
}

[[noreturn]] void statementChild(
    const std::string& path,
    const char* failpoint,
    const std::string& sql,
    std::size_t frames) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::net::DatabaseServer server(path, config(frames));
    if (!sql.empty()) static_cast<void>(server.sqlEngine().execute(sql));
    ::_exit(90);
}

template <typename Function>
void expectCrash(Function&& function, const char* failpoint) {
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) function();
    int status = 0;
    if (::waitpid(child, &status, 0) != child) throw std::runtime_error("waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            std::string("Child did not hit checkpoint failpoint ") + failpoint);
}

std::size_t rowCount(minidb::net::DatabaseServer& server) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM events")).rows.size();
}

void testCheckpointPublicationCrashMatrix() {
    for (const auto* failpoint : {
             "checkpoint_after_begin_append",
             "checkpoint_after_buffer_flush",
             "checkpoint_after_database_sync",
             "checkpoint_after_end_append",
             "checkpoint_after_end_fsync",
             "checkpoint_mid_control_write",
             "checkpoint_after_control_sync"}) {
        minidb::test::TemporaryDatabase database(
            std::string("checkpoint_crash_") + failpoint);
        const auto oldCheckpoint = initialize(database.path().string());
        expectCrash(
            [&] { checkpointChild(database.path().string(), failpoint); }, failpoint);
        minidb::net::DatabaseServer server(database.path().string(), config());
        const auto& recovery = server.startupRecoveryStats();
        const bool published = std::string_view(failpoint) == "checkpoint_after_control_sync";
        require(recovery.checkpointUsed
                    && recovery.checkpointId == oldCheckpoint + (published ? 1 : 0)
                    && rowCount(server) == 2,
                std::string("Checkpoint authority/state crossed crash boundary at ") + failpoint);
        server.catalog().validate();
        server.pageAllocator().validate();
    }
}

void testPostCheckpointWinnerLoserAndInterruptedRecovery() {
    {
        minidb::test::TemporaryDatabase database("checkpoint_winner");
        initialize(database.path().string());
        expectCrash(
            [&] { statementChild(database.path().string(), "after_commit_sync",
                                  "INSERT INTO events VALUES (2, 'winner')", 3); },
            "after_commit_sync");
        expectCrash(
            [&] { statementChild(database.path().string(), "recovery_after_redo_page", "", 3); },
            "recovery_after_redo_page");
        minidb::net::DatabaseServer server(database.path().string(), config());
        require(server.startupRecoveryStats().checkpointUsed && rowCount(server) == 2,
                "Checkpoint-tail winner was not REDOed idempotently");
    }
    {
        minidb::test::TemporaryDatabase database("checkpoint_loser");
        initialize(database.path().string());
        expectCrash(
            [&] { statementChild(database.path().string(), "after_database_page_write",
                                  "INSERT INTO events VALUES (2, 'loser')", 1); },
            "after_database_page_write");
        expectCrash(
            [&] { statementChild(database.path().string(), "recovery_after_undo_page", "", 3); },
            "recovery_after_undo_page");
        minidb::net::DatabaseServer server(database.path().string(), config());
        require(server.startupRecoveryStats().checkpointUsed && rowCount(server) == 1,
                "Checkpoint-tail loser was not UNDOne idempotently");
        server.catalog().validate();
        server.pageAllocator().validate();
    }
}
} // namespace

int main() {
    try {
        testCheckpointPublicationCrashMatrix();
        testPostCheckpointWinnerLoserAndInterruptedRecovery();
        std::cout << "checkpoint_crash_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "checkpoint_crash_test failed: " << error.what() << '\n';
        return 1;
    }
}
