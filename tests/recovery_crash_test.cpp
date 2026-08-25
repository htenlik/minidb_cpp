#include "minidb/database_server.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {

using minidb::test::require;

void initialize(const std::string& path) {
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 4, 2});
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE events (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
}

[[noreturn]] void childWork(
    const std::string& path,
    const char* failpoint,
    bool insert) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 1, 2});
    if (insert) {
        static_cast<void>(server.sqlEngine().execute(
            "INSERT INTO events VALUES (7, 'durable')"));
    }
    ::_exit(90); // The requested failpoint was not reached.
}

void expectCrash(const std::string& path, const char* failpoint, bool insert) {
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) childWork(path, failpoint, insert);
    int status = 0;
    if (::waitpid(child, &status, 0) != child) throw std::runtime_error("waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            std::string("child did not hit failpoint ") + failpoint);
}

std::size_t rowCount(const std::string& path) {
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 3, 2});
    server.catalog().validate();
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM events")).rows.size();
}

void testCommittedCrashRedoAndInterruptedRecovery() {
    minidb::test::TemporaryDatabase database("crash_commit_redo");
    const auto path = database.path().string();
    initialize(path);
    expectCrash(path, "after_commit_sync", true);
    // The first recovery itself crashes after writing one REDO image.
    expectCrash(path, "recovery_after_redo_page", false);
    require(rowCount(path) == 1,
            "Repeated recovery did not reconstruct the committed statement exactly once");
    require(rowCount(path) == 1, "Recovery was not idempotent on a second clean reopen");
}

void testStealCrashUndo() {
    minidb::test::TemporaryDatabase database("crash_steal_undo");
    const auto path = database.path().string();
    initialize(path);
    expectCrash(path, "after_database_page_write", true);
    require(rowCount(path) == 0,
            "Recovery did not undo an uncommitted statement whose page was stolen");
    require(rowCount(path) == 0, "Loser recovery was not idempotent after durable ABORT");
}

} // namespace

int main() {
    try {
        testCommittedCrashRedoAndInterruptedRecovery();
        testStealCrashUndo();
        std::cout << "recovery_crash_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_crash_test failed: " << error.what() << '\n';
        return 1;
    }
}
