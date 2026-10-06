#include "minidb/database_server.hpp"
#include "minidb/sql_error.hpp"
#include "concurrency_utils.hpp"

#include <atomic>
#include <barrier>
#include <iostream>
#include <latch>
#include <thread>
#include <vector>

namespace {
using minidb::test::require;
using minidb::test::await;
using minidb::net::DatabaseServer;
minidb::net::ServerConfig config() {
    minidb::net::ServerConfig result;
    result.port = 0; result.bufferFrames = 3; result.checkpointWalBytes = 0;
    return result;
}
void exec(DatabaseServer& db, const std::string& sql, minidb::SessionId session = 0) {
    static_cast<void>(db.sqlEngine().execute(sql, session));
}
std::uint32_t value(DatabaseServer& db, minidb::SessionId session) {
    const auto result = db.sqlEngine().execute("SELECT value FROM t WHERE id = 1", session);
    return std::get<std::uint32_t>(std::get<minidb::sql::SelectResult>(result).rows.at(0).at(0));
}
void setup(DatabaseServer& db) {
    exec(db, "CREATE TABLE t (id UINT32 PRIMARY KEY, value UINT32)");
    exec(db, "INSERT INTO t VALUES (1, 10)");
    static_cast<void>(db.checkpointManager().checkpoint());
}
void readersAndWriter() {
    minidb::test::TemporaryDatabase path("concurrent_sessions");
    DatabaseServer db(path.path().string(), config()); setup(db);
    auto& engine = db.sqlEngine();
    auto& gate = engine.transactionManager().accessGate();
    gate.resetStats();
    const auto walBefore = db.logManager().lastValidOffset();
    std::latch ready(8), releaseReaders(1), releaseWriter(1);
    minidb::test::ThreadErrors errors;
    std::vector<std::jthread> readers;
    for (minidb::SessionId id = 1; id <= 8; ++id) readers.emplace_back([&, id] { errors.run([&] {
        exec(db, "BEGIN TRANSACTION READ ONLY", id);
        for (const auto source : {"INSERT INTO t VALUES (2, 20)", "UPDATE t SET value = 20",
                                 "DELETE FROM t", "CREATE TABLE forbidden (id UINT32)"}) {
            minidb::test::requireThrows<minidb::sql::SqlExecutionError>([&] { exec(db, source, id); },
                "READ ONLY accepted mutation");
            require(engine.transactionManager().accessMode(id) == minidb::AccessMode::ReadOnly,
                    "Policy rejection ended or upgraded the transaction");
        }
        require(value(db, id) == 10, "Read-only transaction changed data");
        ready.count_down(); releaseReaders.wait();
        for (int query = 0; query < 20; ++query) require(value(db, id) == 10, "Reader snapshot changed");
        exec(db, id % 2 == 0 ? "COMMIT" : "ROLLBACK", id);
        engine.closeSession(id);
    }); });
    ready.wait();
    require(gate.stats().activeReaders == 8 && !db.recoveryCoordinator().hasActiveStatement(),
            "Readers did not coexist or created a recovery writer");
    require(db.logManager().lastValidOffset() == walBefore, "Read-only policy violations generated WAL");
    std::atomic<bool> writerEntered{false}, lateReaderEntered{false};
    std::jthread writer([&] { errors.run([&] {
        exec(db, "BEGIN READ WRITE", 20); writerEntered = true;
        exec(db, "UPDATE t SET value = 20 WHERE id = 1", 20);
        releaseWriter.wait(); exec(db, "COMMIT", 20); engine.closeSession(20);
    }); });
    await([&] { return gate.stats().waitingWriters == 1; }, "Writer bypassed read-only transactions");
    std::jthread lateReader([&] { errors.run([&] {
        require(value(db, 30) == 20, "Late reader did not see committed writer state");
        lateReaderEntered = true; engine.closeSession(30);
    }); });
    await([&] { return gate.stats().waitingReaders == 1; }, "Late reader bypassed waiting writer");
    require(!writerEntered && !lateReaderEntered, "Access exclusion failed");
    releaseReaders.count_down();
    await([&] { return writerEntered.load(); }, "Readers did not release writer");
    require(!lateReaderEntered, "Reader saw active writer");
    releaseWriter.count_down();
    for (auto& reader : readers) reader.join();
    writer.join(); lateReader.join(); errors.rethrow();
    require(!engine.transactionManager().hasActiveExplicitTransaction(), "Session state leaked");
    db.catalog().validate(); db.pageAllocator().validate(); db.bufferPool().validate();
}
void writerExclusion() {
    for (const auto terminal : {"COMMIT", "ROLLBACK"}) {
        minidb::test::TemporaryDatabase path("writer_exclusion");
        DatabaseServer db(path.path().string(), config()); setup(db);
        exec(db, "BEGIN", 40); exec(db, "UPDATE t SET value = 99 WHERE id = 1", 40);
        auto& gate = db.sqlEngine().transactionManager().accessGate();
        minidb::test::ThreadErrors errors;
        std::atomic<bool> read{false};
        std::jthread reader([&] { errors.run([&] {
            require(value(db, 41) == (std::string_view(terminal) == "COMMIT" ? 99U : 10U),
                    "Reader observed intermediate or incorrect terminal state");
            read = true; db.sqlEngine().closeSession(41);
        }); });
        await([&] { return gate.stats().waitingReaders == 1; }, "Writer did not block reader");
        require(!read, "Reader completed while writer active");
        exec(db, terminal, 40); reader.join(); errors.rethrow();

        exec(db, "BEGIN", 40);
        std::atomic<bool> second{false};
        std::jthread other([&] { errors.run([&] {
            exec(db, "BEGIN", 42); second = true;
            exec(db, "UPDATE t SET value = 77 WHERE id = 1", 42);
            exec(db, "ROLLBACK", 42); db.sqlEngine().closeSession(42);
        }); });
        await([&] { return gate.stats().waitingWriters == 1; }, "Second writer did not wait");
        require(!second, "Two writer recovery contexts existed");
        exec(db, "ROLLBACK", 40); other.join(); errors.rethrow();
        require(second && !db.recoveryCoordinator().hasActiveStatement(), "Writer handoff failed");
    }
}
void checkpointFairness() {
    for (auto mode : {minidb::CheckpointMode::Sharp, minidb::CheckpointMode::Fuzzy}) {
        minidb::test::TemporaryDatabase path("checkpoint_readers");
        DatabaseServer db(path.path().string(), config()); setup(db);
        exec(db, "BEGIN READ ONLY", 50);
        auto& gate = db.sqlEngine().transactionManager().accessGate();
        const auto before = db.checkpointManager().stats().checkpointsCompleted;
        minidb::test::ThreadErrors errors;
        std::jthread checkpoint([&] { errors.run([&] { static_cast<void>(db.checkpointManager().checkpoint(mode)); }); });
        await([&] { return gate.stats().waitingWriters == 1; }, "Checkpoint bypassed active READ ONLY scope");
        std::vector<std::jthread> readers;
        for (minidb::SessionId session = 60; session < 68; ++session) readers.emplace_back([&, session] { errors.run([&] {
            require(value(db, session) == 10 && db.checkpointManager().stats().checkpointsCompleted > before,
                    "New readers starved checkpoint or overlapped publication");
            db.sqlEngine().closeSession(session);
        }); });
        await([&] { return gate.stats().waitingReaders == 8; }, "Reader stream bypassed queued checkpoint");
        exec(db, "COMMIT", 50); checkpoint.join();
        for (auto& reader : readers) reader.join();
        errors.rethrow();
        require(db.checkpointManager().stats().activeTransactionsCaptured == 0, "Runtime checkpoint captured ATT");
    }
}
}
int main() {
    try { readersAndWriter(); writerExclusion(); checkpointFairness();
        std::cout << "concurrent_sessions_test passed (8 READ ONLY transactions, reader/writer exclusion, checkpoint fairness)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
