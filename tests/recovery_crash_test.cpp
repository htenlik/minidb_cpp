#include "minidb/database_server.hpp"
#include "minidb/minidb_client.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
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
    const std::string& sql) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 1, 2});
    if (!sql.empty()) static_cast<void>(server.sqlEngine().execute(sql));
    ::_exit(90); // The requested failpoint was not reached.
}

void expectCrash(
    const std::string& path,
    const char* failpoint,
    std::string sql = {}) {
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) childWork(path, failpoint, sql);
    int status = 0;
    if (::waitpid(child, &status, 0) != child) throw std::runtime_error("waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            std::string("child did not hit failpoint ") + failpoint
                + "; wait status=" + std::to_string(status));
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
    expectCrash(path, "after_commit_sync", "INSERT INTO events VALUES (7, 'durable')");
    // The first recovery itself crashes after writing one REDO image.
    expectCrash(path, "recovery_after_redo_page");
    require(rowCount(path) == 1,
            "Repeated recovery did not reconstruct the committed statement exactly once");
    require(rowCount(path) == 1, "Recovery was not idempotent on a second clean reopen");
}

void testStealCrashUndo() {
    minidb::test::TemporaryDatabase database("crash_steal_undo");
    const auto path = database.path().string();
    initialize(path);
    expectCrash(path, "after_database_page_write", "INSERT INTO events VALUES (7, 'durable')");
    require(rowCount(path) == 0,
            "Recovery did not undo an uncommitted statement whose page was stolen");
    require(rowCount(path) == 0, "Loser recovery was not idempotent after durable ABORT");
}

std::vector<minidb::RowValues> query(const std::string& path, const std::string& sql) {
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 3, 2});
    server.catalog().validate();
    return std::get<minidb::sql::SelectResult>(server.sqlEngine().execute(sql)).rows;
}

void testCreateCrashBoundary() {
    for (const auto& [failpoint, shouldExist] : {
             std::pair{"before_commit_append", false},
             std::pair{"after_commit_sync", true}}) {
        minidb::test::TemporaryDatabase database(
            shouldExist ? "create_after_commit" : "create_before_commit");
        const auto path = database.path().string();
        {
            minidb::net::DatabaseServer bootstrap(path, {"127.0.0.1", 0, 8, 3, 2});
        }
        expectCrash(
            path, failpoint,
            "CREATE TABLE created (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)");
        minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 3, 2});
        require(server.catalog().findTable("created").has_value() == shouldExist,
                "CREATE visibility disagrees with durable COMMIT boundary");
        server.catalog().validate();
    }
}

void initializeUpdateDatabase(const std::string& path) {
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 4, 2});
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE docs (id UINT32 PRIMARY KEY, body VARCHAR(3000) NOT NULL)"));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO docs VALUES (1, 'old')"));
    static_cast<void>(server.sqlEngine().execute("INSERT INTO docs VALUES (2, 'filler')"));
}

void testUpdatePrimaryKeyAndDeleteCrashBoundaries() {
    const std::string largeValue(2500, 'x');
    for (const auto& [failpoint, committed] : {
             std::pair{"before_commit_append", false},
             std::pair{"after_commit_sync", true}}) {
        minidb::test::TemporaryDatabase updateDb(
            committed ? "update_after_commit" : "update_before_commit");
        const auto updatePath = updateDb.path().string();
        initializeUpdateDatabase(updatePath);
        expectCrash(
            updatePath, failpoint,
            "UPDATE docs SET body = '" + largeValue + "' WHERE id = 1");
        const auto updated = query(updatePath, "SELECT body FROM docs WHERE id = 1");
        require(updated.size() == 1
                    && std::get<std::string>(updated[0][0])
                        == (committed ? largeValue : std::string("old")),
                "Large tuple update mixed pre/post-transaction state");

        minidb::test::TemporaryDatabase pkDb(
            committed ? "pk_after_commit" : "pk_before_commit");
        const auto pkPath = pkDb.path().string();
        initializeUpdateDatabase(pkPath);
        expectCrash(pkPath, failpoint, "UPDATE docs SET id = 9 WHERE id = 1");
        require(query(pkPath, "SELECT * FROM docs WHERE id = 1").size()
                        == (committed ? 0U : 1U)
                    && query(pkPath, "SELECT * FROM docs WHERE id = 9").size()
                        == (committed ? 1U : 0U),
                "Primary-key change mixed old and new index state");

        minidb::test::TemporaryDatabase deleteDb(
            committed ? "delete_after_commit" : "delete_before_commit");
        const auto deletePath = deleteDb.path().string();
        initializeUpdateDatabase(deletePath);
        expectCrash(deletePath, failpoint, "DELETE FROM docs WHERE id = 1");
        require(query(deletePath, "SELECT * FROM docs WHERE id = 1").size()
                        == (committed ? 0U : 1U),
                "DELETE visibility disagrees with durable COMMIT boundary");
    }
}

void initializeTreeBoundary(
    const std::string& path,
    std::uint32_t keyCount,
    bool removeFirst) {
    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 8, 2});
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE keys (id UINT32 PRIMARY KEY, value VARCHAR(8) NOT NULL)"));
    for (std::uint32_t key = 0; key < keyCount; ++key) {
        static_cast<void>(server.sqlEngine().execute(
            "INSERT INTO keys VALUES (" + std::to_string(key) + ", 'v')"));
    }
    if (removeFirst) {
        static_cast<void>(server.sqlEngine().execute("DELETE FROM keys WHERE id = 0"));
    }
}

void testBPlusSplitMergeAndFreeListCrash() {
    for (const auto& [failpoint, committed] : {
             std::pair{"before_commit_append", false},
             std::pair{"after_commit_sync", true}}) {
        minidb::test::TemporaryDatabase splitDatabase(
            committed ? "tree_split_after" : "tree_split_before");
        const auto splitPath = splitDatabase.path().string();
        initializeTreeBoundary(splitPath, 406, false);
        expectCrash(splitPath, failpoint, "INSERT INTO keys VALUES (406, 'v')");
        require(query(splitPath, "SELECT * FROM keys WHERE id = 406").size()
                        == (committed ? 1U : 0U),
                "B+ leaf/root split recovery crossed commit boundary");
        {
            minidb::net::DatabaseServer validation(
                splitPath, {"127.0.0.1", 0, 8, 4, 2});
            validation.catalog().validate();
            validation.pageAllocator().validate();
        }

        // 407 entries produce two leaves at physical capacity 406. Deleting 0 then 1
        // drives redistribution followed by merge and free-list reclamation.
        minidb::test::TemporaryDatabase database(
            committed ? "tree_merge_after" : "tree_merge_before");
        const auto path = database.path().string();
        initializeTreeBoundary(path, 407, true);
        expectCrash(path, failpoint, "DELETE FROM keys WHERE id = 1");
        require(query(path, "SELECT * FROM keys WHERE id = 1").size()
                        == (committed ? 0U : 1U),
                "B+ merge/root/free-list crash recovery crossed commit boundary");
        minidb::net::DatabaseServer validation(path, {"127.0.0.1", 0, 8, 4, 2});
        validation.catalog().validate();
        validation.pageAllocator().validate();
    }
}

void testTcpCrashRestartCommitBoundary() {
    minidb::test::TemporaryDatabase database("tcp_crash_restart");
    const auto path = database.path().string();
    initialize(path);
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        ::setenv("MINIDB_FAILPOINT", "after_commit_sync", 1);
        minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 3, 2});
        server.start();
        std::thread serving([&] { server.serve(1); });
        minidb::net::MiniDbClient client("127.0.0.1", server.port());
        client.connect();
        client.handshake();
        static_cast<void>(client.execute("INSERT INTO events VALUES (42, 'tcp')"));
        serving.join();
        ::_exit(90);
    }
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "TCP worker did not crash after durable COMMIT");

    minidb::net::DatabaseServer server(path, {"127.0.0.1", 0, 8, 3, 2});
    server.start();
    std::thread serving([&] { server.serve(1); });
    minidb::net::MiniDbClient client("127.0.0.1", server.port());
    client.connect();
    client.handshake();
    const auto result = client.execute("SELECT * FROM events WHERE id = 42");
    client.close();
    serving.join();
    require(std::get<minidb::sql::SelectResult>(result).rows.size() == 1,
            "Restarted TCP server did not expose durable-commit recovery state");
    server.catalog().validate();
}

} // namespace

int main() {
    try {
        testCommittedCrashRedoAndInterruptedRecovery();
        testStealCrashUndo();
        testCreateCrashBoundary();
        testUpdatePrimaryKeyAndDeleteCrashBoundaries();
        testBPlusSplitMergeAndFreeListCrash();
        testTcpCrashRestartCommitBoundary();
        std::cout << "recovery_crash_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_crash_test failed: " << error.what() << '\n';
        return 1;
    }
}
