#include "minidb/database_server.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/sql_error.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using minidb::test::require;
using minidb::net::DatabaseServer;

minidb::net::ServerConfig config(minidb::WalUpdateMode mode, std::size_t frames = 4) {
    minidb::net::ServerConfig result{"127.0.0.1", 0, 8, frames, 2};
    result.checkpointWalBytes = 0;
    result.walSegmentBytes = 16384;
    result.walUpdateMode = mode;
    return result;
}
void exec(DatabaseServer& server, const std::string& source) {
    static_cast<void>(server.sqlEngine().execute(source));
}
std::vector<minidb::RowValues> rows(DatabaseServer& server, const std::string& source) {
    return std::get<minidb::sql::SelectResult>(server.sqlEngine().execute(source)).rows;
}
void rejects(DatabaseServer& server, const std::string& source) {
    minidb::test::requireThrows<minidb::sql::SqlExecutionError>(
        [&] { exec(server, source); }, "Expected controlled transaction error: " + source);
}
void validate(DatabaseServer& server) {
    server.catalog().validate();
    server.pageAllocator().validate();
    server.bufferPool().validate();
    for (const auto& table : server.catalog().listTables()) {
        server.catalog().openTable(table.tableId).validate();
    }
}
void create(DatabaseServer& server) {
    exec(server, "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
    exec(server, "INSERT INTO t VALUES (1, 'original')");
}

void semantics(minidb::WalUpdateMode mode) {
    minidb::test::TemporaryDatabase db("explicit_semantics");
    minidb::TransactionId committedId = 0;
    {
        DatabaseServer server(db.path().string(), config(mode));
        create(server);
        auto& engine = server.sqlEngine();
        auto& manager = engine.transactionManager();
        server.bufferPool().flushAll(); // Exclude eviction of setup's NO-FORCE winner pages.
        const auto walBefore = server.logManager().lastValidOffset();
        const auto writesBefore = server.bufferPool().stats().physicalPageWrites;
        for (const auto terminal : {"COMMIT", "ROLLBACK"}) {
            const auto result = std::get<minidb::sql::CommandResult>(engine.execute("BEGIN TRANSACTION;"));
            require(result.command == minidb::sql::CommandKind::Begin && result.affectedRows == 0,
                    "BEGIN command response mismatch");
            require(manager.hasActiveExplicitTransaction() && !manager.hasMaterializedWalBegin(),
                    "BEGIN was not lazy");
            rejects(server, "BEGIN");
            rejects(server, "SELECT * FROM nonexistent");
            require(manager.hasActiveExplicitTransaction(), "Read/nested BEGIN error ended transaction");
            require(rows(server, "SELECT * FROM t").size() == 1, "Read-only transaction failed");
            exec(server, terminal);
        }
        require(server.logManager().lastValidOffset() == walBefore
                    && server.bufferPool().stats().physicalPageWrites == writesBefore,
                "Read-only transaction generated WAL/database writes");
        require(manager.stats().readOnlyExplicitTransactions == 2, "Read-only metric wrong");
        rejects(server, "COMMIT");
        rejects(server, "ROLLBACK");

        exec(server, "BEGIN");
        const auto id = manager.activeTransactionId();
        const auto startPages = server.diskManager().pageCount();
        exec(server, "INSERT INTO t VALUES (2, 'new')");
        const auto firstLsn = manager.lastLsn();
        exec(server, "UPDATE t SET value = 'second' WHERE id = 1");
        exec(server, "UPDATE t SET value = 'third' WHERE id = 1");
        exec(server, "UPDATE t SET value = 'fourth', id = 9 WHERE id = 1");
        require(manager.activeTransactionId() == id && manager.lastLsn() > firstLsn,
                "Identity/prevLSN lifetime did not span statements");
        require(rows(server, "SELECT value FROM t WHERE id = 9")
                    == std::vector<minidb::RowValues>{{std::string("fourth")}},
                "Read-your-writes after PK update failed");
        exec(server, "DELETE FROM t WHERE id = 2");
        server.logManager().flushAll();
        const auto records = server.logManager().scan().records;
        std::size_t begins = 0;
        auto prev = minidb::INVALID_LSN;
        for (const auto& record : records) {
            if (record.transactionId != id) continue;
            if (record.type == minidb::LogRecordType::Begin) {
                ++begins;
                require(minidb::decodeBeginLogPayload(record.payload).startPageCount == startPages,
                        "Transaction page boundary changed");
            }
            require(record.prevLsn == prev && record.type != minidb::LogRecordType::Commit,
                    "Intermediate COMMIT or broken statement-spanning chain");
            prev = record.lsn;
        }
        require(begins == 1, "Explicit scope did not contain exactly one WAL BEGIN");
        exec(server, "ROLLBACK");
        require(rows(server, "SELECT * FROM t")
                    == std::vector<minidb::RowValues>{{std::uint32_t{1}, std::string("original")}},
                "Whole-scope rollback did not restore original state");
        require(server.recoveryCoordinator().lastRollbackStats().clrsAppended > 0
                    && server.recoveryCoordinator().lastRollbackStats().durableAbortObserved,
                "SQL ROLLBACK did not use durable CLRs/ABORT");

        exec(server, "BEGIN");
        exec(server, "INSERT INTO t VALUES (2, 'new')");
        rejects(server, "INSERT INTO t VALUES (1, 'duplicate')");
        require(!manager.hasActiveExplicitTransaction() && rows(server, "SELECT * FROM t").size() == 1,
                "Mutating error did not roll entire explicit scope back");

        exec(server, "BEGIN");
        exec(server, "CREATE TABLE transient (id UINT32 PRIMARY KEY)");
        exec(server, "INSERT INTO transient VALUES (3)");
        exec(server, "ROLLBACK");
        require(!server.catalog().findTable("transient"), "Transactional CREATE did not roll back");
        exec(server, "BEGIN");
        exec(server, "CREATE TABLE durable (id UINT32 PRIMARY KEY)");
        exec(server, "INSERT INTO durable VALUES (3)");
        exec(server, "COMMIT");

        // Enough frames for the final commit: it must not force dirty database pages.
        exec(server, "BEGIN");
        committedId = manager.activeTransactionId();
        exec(server, "UPDATE t SET value = 'a' WHERE id = 1");
        exec(server, "UPDATE t SET value = 'committed', id = 8 WHERE id = 1");
        const auto beforeCommitWrites = server.bufferPool().stats().physicalPageWrites;
        exec(server, "COMMIT");
        require(server.bufferPool().stats().physicalPageWrites == beforeCommitWrites,
                "Explicit COMMIT forced database pages");
        validate(server);
    }
    {
        DatabaseServer server(db.path().string(), config(mode));
        require(rows(server, "SELECT value FROM t WHERE id = 8")
                    == std::vector<minidb::RowValues>{{std::string("committed")}},
                "Explicit commit did not survive reopen");
        require(server.catalog().findTable("durable").has_value(), "CREATE commit lost");
        exec(server, "BEGIN");
        require(server.sqlEngine().transactionManager().activeTransactionId() > committedId,
                "Logged transaction IDs were reused after reopen");
        exec(server, "ROLLBACK");
        validate(server);
    }
}

void ownershipAndShutdown() {
    minidb::test::TemporaryDatabase db("explicit_ownership");
    {
        DatabaseServer server(db.path().string(), config(minidb::WalUpdateMode::Adaptive));
        create(server);
        auto& engine = server.sqlEngine();
        static_cast<void>(engine.execute("BEGIN", 31));
        static_cast<void>(engine.execute("INSERT INTO t VALUES (2, 'uncommitted')", 31));
        for (const auto sql : {"BEGIN", "COMMIT", "ROLLBACK", "SELECT * FROM t",
                               "INSERT INTO t VALUES (3, 'wrong owner')"}) {
            minidb::test::requireThrows<minidb::sql::SqlExecutionError>(
                [&] { static_cast<void>(engine.execute(sql, 32)); }, "Session crossed ownership");
        }
        engine.closeSession(32);
        require(engine.transactionManager().hasActiveExplicitTransaction(), "Wrong disconnect ended owner");
        server.close();
        require(!engine.transactionManager().hasActiveExplicitTransaction(), "Shutdown did not roll back");
    }
    DatabaseServer reopened(db.path().string(), config(minidb::WalUpdateMode::Adaptive));
    require(rows(reopened, "SELECT * FROM t").size() == 1, "Shutdown rollback was not durable");
}

void checkpointDeferral() {
    for (auto mode : {minidb::CheckpointMode::Sharp, minidb::CheckpointMode::Fuzzy}) {
        for (const auto terminal : {"COMMIT", "ROLLBACK"}) {
            minidb::test::TemporaryDatabase db("explicit_checkpoint");
            auto settings = config(minidb::WalUpdateMode::Adaptive);
            settings.checkpointStatements = 2;
            settings.checkpointWalBytes = 100;
            settings.checkpointMode = mode;
            DatabaseServer server(db.path().string(), settings);
            create(server);
            const auto checkpointCount = server.checkpointManager().stats().checkpointsCompleted;
            const auto deleted = server.logManager().stats().segmentsDeleted;
            exec(server, "BEGIN");
            for (int index = 0; index < 10; ++index) {
                exec(server, "UPDATE t SET value = 'value" + std::to_string(index) + "' WHERE id = 1");
            }
            require(server.checkpointManager().pending(), "Checkpoint trigger was lost during scope");
            require(server.checkpointManager().stats().checkpointsCompleted == checkpointCount
                        && server.logManager().stats().segmentsDeleted == deleted,
                    "Checkpoint/reclamation ran while explicit transaction active");
            for (auto requested : {minidb::CheckpointMode::Sharp, minidb::CheckpointMode::Fuzzy}) {
                minidb::test::requireThrows<std::logic_error>([&] {
                    static_cast<void>(server.checkpointManager().checkpoint(requested));
                }, "Manual active checkpoint succeeded");
            }
            exec(server, terminal);
            require(!server.checkpointManager().pending()
                        && server.checkpointManager().stats().checkpointsCompleted == checkpointCount + 1
                        && server.checkpointManager().stats().activeTransactionsCaptured == 0,
                    "Deferred checkpoint not completed with empty ATT");
            validate(server);
        }
    }
}

void thousandStatementsAndStructures() {
    minidb::test::TemporaryDatabase db("explicit_thousand");
    DatabaseServer server(db.path().string(), config(minidb::WalUpdateMode::Adaptive));
    exec(server, "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
    const auto emptyPages = server.diskManager().pageCount();
    for (const auto terminal : {"ROLLBACK", "COMMIT"}) {
        exec(server, "BEGIN");
        for (int key = 0; key < 1000; ++key) {
            exec(server, "INSERT INTO t VALUES (" + std::to_string(key) + ", 'v')");
        }
        require(rows(server, "SELECT * FROM t").size() == 1000, "1,000 statements lost tuples");
        const auto bytes = server.sqlEngine().transactionManager().transactionWalBytes();
        const auto touched = server.sqlEngine().transactionManager().touchedPageCount();
        const auto started = std::chrono::steady_clock::now();
        exec(server, terminal);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        const bool committed = std::string_view(terminal) == "COMMIT";
        require(rows(server, "SELECT * FROM t").size() == (committed ? 1000U : 0U),
                "1,000-statement terminal state mismatch");
        if (!committed) require(server.diskManager().pageCount() == emptyPages, "Appended pages not truncated");
        std::cout << "1000 statements " << terminal << ": wal=" << bytes << " touched=" << touched
                  << " terminal_ns=" << ns << " clr_bytes="
                  << (committed ? 0 : server.recoveryCoordinator().lastRollbackStats().undoWalBytes) << '\n';
        validate(server);
    }
    const auto baseline = server.catalog().openTable("t").scan();
    exec(server, "BEGIN");
    exec(server, "DELETE FROM t WHERE id < 999");
    const std::string payload(3000, 'R');
    exec(server, "UPDATE t SET value = '" + payload + "' WHERE id = 999");
    for (int key = 1000; key < 1450; ++key) {
        exec(server, "INSERT INTO t VALUES (" + std::to_string(key) + ", 'reuse')");
    }
    validate(server);
    exec(server, "ROLLBACK");
    require(server.catalog().openTable("t").scan() == baseline,
            "Merge/root shrink/free-page reuse rollback changed RIDs or contents");
    validate(server);
}

void crashAndRelocation(minidb::WalUpdateMode mode) {
    for (const auto point : {"before_explicit_commit", "after_commit_append", "after_commit_sync",
                             "recovery_after_clr_wal_force", "recovery_after_compensation_page_write"}) {
        minidb::test::TemporaryDatabase db("explicit_crash");
        {
            DatabaseServer server(db.path().string(), config(mode));
            create(server);
            for (int key = 2; key <= 20; ++key) {
                exec(server, "INSERT INTO t VALUES (" + std::to_string(key) + ", '"
                    + std::string(160, 'x') + "')");
            }
        }
        const auto pid = ::fork();
        require(pid >= 0, "fork failed");
        if (pid == 0) {
            try {
                DatabaseServer server(db.path().string(), config(mode, 2));
                exec(server, "BEGIN");
                exec(server, "UPDATE t SET value = '" + std::string(3900, 'L') + "', id = 100 WHERE id = 1");
                exec(server, "DELETE FROM t WHERE id = 2");
                exec(server, "INSERT INTO t VALUES (101, 'appended')");
                server.bufferPool().flushAll(); // Explicit STEAL between SQL statements.
                server.diskManager().sync();
                if (std::string_view(point) == "before_explicit_commit") ::_exit(86);
                ::setenv("MINIDB_FAILPOINT", point, 1);
                exec(server, std::string_view(point).starts_with("recovery_") ? "ROLLBACK" : "COMMIT");
                ::_exit(90);
            } catch (...) { ::_exit(91); }
        }
        int status = 0;
        require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 86,
                std::string("Crash point failed: ") + point + " status=" + std::to_string(status));
        DatabaseServer reopened(db.path().string(), config(mode));
        const bool committed = std::string_view(point) == "after_commit_sync";
        require(rows(reopened, "SELECT * FROM t WHERE id = 1").size() == (committed ? 0U : 1U)
                    && rows(reopened, "SELECT * FROM t WHERE id = 100").size() == (committed ? 1U : 0U)
                    && rows(reopened, "SELECT * FROM t WHERE id = 2").size() == (committed ? 0U : 1U),
                std::string("Explicit atomicity/relocation recovery failed: ") + point);
        if (committed) require(rows(reopened, "SELECT value FROM t WHERE id = 100")
                                   == std::vector<minidb::RowValues>{{std::string(3900, 'L')}},
                               "Committed relocated bytes incorrect");
        else require(rows(reopened, "SELECT value FROM t WHERE id = 1")
                         == std::vector<minidb::RowValues>{{std::string("original")}},
                     "Rollback did not restore exact original tuple");
        validate(reopened);
    }
}
} // namespace

int main() {
    try {
        for (auto mode : {minidb::WalUpdateMode::FullPage, minidb::WalUpdateMode::ByteRange,
                          minidb::WalUpdateMode::Adaptive}) {
            semantics(mode);
            crashAndRelocation(mode);
        }
        ownershipAndShutdown();
        checkpointDeferral();
        thousandStatementsAndStructures();
        std::cout << "explicit_transaction_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "explicit_transaction_test failed: " << error.what() << '\n';
        return 1;
    }
}
