#include "owned_rollback_fixture.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/catalog.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/sql_parser.hpp"
#include "minidb/table.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/tcp_server.hpp"
#include "minidb/minidb_client.hpp"

#include <iostream>
#include <fstream>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace minidb::test {
// No production callbacks or switches: this harness uses the actual manager's
// admission/completion and executor, inserting only private authority binding
// between admission and the first mutation. All control/read SQL goes through
// SqlEngine::execute unchanged. Implicit mutation dispatch mirrors that method.
class OwnedSqlTestAccess {
public:
    static void bind(sql::SqlEngine& engine, SessionId session = LOCAL_SESSION_ID) {
        auto& manager = engine.transactions_;
        const auto context = manager.findSession(session);
        require(context != nullptr && context->lease.has_value(), "SQL session has no lease");
        std::lock_guard lock(context->mutex);
        OwnedRollbackTestAccess::bindExisting(*manager.recovery_, manager.gate_,
            *context->lease, context->writerId);
    }
    static sql::QueryResult execute(sql::SqlEngine& engine, std::string_view source,
                                    SessionId session = LOCAL_SESSION_ID) {
        const auto statement = sql::Parser::parse(source);
        if (std::holds_alternative<sql::SelectStatement>(statement.node)
            || std::holds_alternative<sql::BeginStatement>(statement.node)
            || std::holds_alternative<sql::CommitStatement>(statement.node)
            || std::holds_alternative<sql::RollbackStatement>(statement.node)) return engine.execute(statement, session);
        auto& manager = engine.transactions_;
        auto guard = manager.lockSession(session);
        manager.requireSession(session);
        manager.beginMutation(session);
        try {
            bind(engine, session);
            auto result = engine.executor_.execute(statement);
            manager.completeMutation(session);
            return result;
        } catch (...) { manager.failMutation(session); throw; }
    }
};
}
namespace {
using namespace minidb;
using test::require;
struct Storage : test::OwnedFixture {
    CheckpointManager checkpoints;
    Catalog catalog;
    sql::SqlEngine engine;
    Storage(const std::string& path, WalUpdateMode mode = WalUpdateMode::Adaptive,
            WalStorageMode store = WalStorageMode::Segmented,
            RedoPolicy policy = RedoPolicy::PageLsnSelectiveRedo)
        : OwnedFixture(path, mode, store, policy),
          checkpoints(recovery, pool, disk, log, control, startup),
          catalog(setup()), engine(catalog, &recovery, &checkpoints) {}
    Catalog setup() {
        if (disk.databaseHeader().catalogRootPageId != INVALID_PAGE_ID) return Catalog::open(pool, disk, allocator);
        recovery.beginStatement();
        auto result = Catalog::openOrCreate(pool, disk, allocator);
        recovery.commitStatement();
        return result;
    }
    void exec(const std::string& source, SessionId session = LOCAL_SESSION_ID) {
        static_cast<void>(test::OwnedSqlTestAccess::execute(engine, source, session));
    }
    sql::SelectResult rows() { return std::get<sql::SelectResult>(engine.execute("SELECT * FROM t")); }
    void validate() {
        catalog.validate(); allocator.validate(); pool.validate();
        for (const auto& definition : catalog.listTables()) catalog.openTable(definition.tableId).validate();
    }
    void create() {
        exec("CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
        exec("BEGIN");
        test::OwnedSqlTestAccess::bind(engine);
        const auto index = catalog.findTable("t")->primaryIndexMetadataPageId;
        {auto page = requireWritePage(pool, index, "small logical index capacities");
         // Existing valid persisted testing knobs, changed under ordinary WAL.
         byte_codec::writeUint32(page.data(), persistent_index_metadata_layout::LEAF_MAX_KEYS_OFFSET, 3);
         byte_codec::writeUint32(page.data(), persistent_index_metadata_layout::INTERNAL_MAX_KEYS_OFFSET, 3);}
        exec("COMMIT");
        exec("BEGIN");
        for (unsigned i = 0; i < 80; ++i)
            exec("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + std::string(60, 'a' + i % 20) + "')");
        exec("COMMIT");
        pool.flushAll(); disk.sync(); validate();
    }
};
void directed() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto store : {WalStorageMode::LegacySingleFile, WalStorageMode::Segmented}) {
        {test::TemporaryDatabase db("owned_catalog_root");
         test::OwnedFixture f(db.path().string(), mode, store); f.begin();
         {auto catalog = Catalog::openOrCreate(f.pool, f.disk, f.allocator); catalog.validate();}
         f.pool.flushAll(); const auto extent = f.disk.pageCount(); f.recovery.rollbackStatement();
         std::vector<PageId> expectedFree;
         for (std::uint64_t page = 1; page < extent; ++page) expectedFree.insert(expectedFree.begin(), static_cast<PageId>(page));
         require(f.disk.pageCount() == extent && f.disk.databaseHeader().catalogRootPageId == INVALID_PAGE_ID
            && f.allocator.freePageIds() == expectedFree, "Initial catalog root rollback lost metadata/pages");}
        test::TemporaryDatabase db("owned_sql");
        sql::SelectResult expected;
        PageId catalogRoot = INVALID_PAGE_ID;
        {Storage f(db.path().string(), mode, store); f.create(); expected = f.rows();
         catalogRoot = f.disk.databaseHeader().catalogRootPageId;
         const auto definition = *f.catalog.findTable("t");
         const auto originalIndex = PersistentBPlusTree::open(f.pool, f.disk, f.allocator, definition.primaryIndexMetadataPageId);
         const auto root = originalIndex.rootPageId();
         for (unsigned scenario = 0; scenario < 6; ++scenario) {
            f.exec("BEGIN");
            if (scenario == 0) for (unsigned i = 80; i < 160; ++i)
                f.exec("INSERT INTO t VALUES (" + std::to_string(i) + ", 'new')");
            if (scenario == 1) {
                f.exec("UPDATE t SET value = '" + std::string(3500, 'X') + "' WHERE id = 0");
                require(f.catalog.openTable("t").findByPrimaryKey(0)->recordId != expected.recordIds.front(),
                    "Large VARCHAR update did not exercise relocation");
            }
            if (scenario == 2) {
                f.exec("DELETE FROM t");
                require(PersistentBPlusTree::open(f.pool, f.disk, f.allocator, definition.primaryIndexMetadataPageId).empty(),
                    "Delete did not empty/shrink index");
            }
            if (scenario == 3) {
                f.exec("DELETE FROM t WHERE id = 1");
                f.exec("UPDATE t SET id = 900 WHERE id = 2");
                f.exec("INSERT INTO t VALUES (1000, 'mixed')");
            }
            if (scenario >= 4) {f.exec("CREATE TABLE transient (id UINT32 PRIMARY KEY)");
                f.exec("INSERT INTO transient VALUES (1)");}
            f.pool.flushAll(); // Exercise STEAL and real on-disk CLR compensation.
            const auto extent = f.disk.pageCount();
            if (scenario == 5) f.engine.closeSession(LOCAL_SESSION_ID);
            else f.exec("ROLLBACK");
            const auto actual = f.rows();
            require(actual.rows == expected.rows && actual.recordIds == expected.recordIds,
                "Owned SQL rollback changed committed contents/RIDs scenario=" + std::to_string(scenario));
            require(f.disk.pageCount() == extent && f.recovery.lastRollbackStats().pagesTruncated == 0,
                "Owned SQL rollback truncated extent");
            require(!f.catalog.findTable("transient") && f.disk.databaseHeader().catalogRootPageId == catalogRoot,
                "Owned SQL rollback damaged catalog");
            require(PersistentBPlusTree::open(f.pool, f.disk, f.allocator, definition.primaryIndexMetadataPageId).rootPageId() == root,
                "Rollback lost original B+ root");
            f.validate();
         }
         f.exec("BEGIN"); f.exec("INSERT INTO t VALUES (800, 'pending')");
         test::requireThrows<sql::SqlExecutionError>([&]{f.exec("INSERT INTO t VALUES (0, 'duplicate')");},
            "Duplicate mutating statement did not fail");
         require(!f.engine.transactionManager().hasActiveExplicitTransaction() && f.rows().rows == expected.rows,
            "Failed statement did not rollback explicit transaction");
         // Implicit mutation error follows the same manager's failMutation path.
         test::requireThrows<sql::SqlExecutionError>([&]{f.exec("INSERT INTO t VALUES (0, 'duplicate')");},
            "Implicit mutating error did not fail");
         f.exec("CREATE TABLE durable (id UINT32 PRIMARY KEY)");
         f.exec("BEGIN"); f.exec("INSERT INTO durable VALUES (3)"); f.exec("COMMIT");
         f.validate(); f.pool.flushAll(); f.disk.sync();}
        {Storage f(db.path().string(), mode, store);
         require(f.rows().rows == expected.rows && f.rows().recordIds == expected.recordIds,
            "Committed SQL contents/RIDs changed after reopen");
         require(f.catalog.openTable("durable").size() == 1 && f.disk.databaseHeader().catalogRootPageId == catalogRoot,
            "Committed CREATE/INSERT lost after reopen"); f.validate();}
      }
}
void implicitFailure() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto store : {WalStorageMode::LegacySingleFile, WalStorageMode::Segmented}) {
        test::TemporaryDatabase db("owned_implicit_append_failure");
        Storage f(db.path().string(), mode, store);
        f.exec("CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
        const auto before = f.disk.pageCount();
        ::setenv("MINIDB_THROWPOINT", "append_after_physical_extension", 1);
        bool rejected = false;
        try {f.exec("INSERT INTO t VALUES (1, 'first')");}
        catch (const std::exception&) {rejected = true;}
        ::unsetenv("MINIDB_THROWPOINT");
        require(rejected && f.rows().rows.empty() && !f.recovery.hasActiveStatement(),
            "Implicit allocation failure did not rollback exact logical state");
        require(f.disk.pageCount() == before + 1
            && f.allocator.freePageIds() == std::vector<PageId>{static_cast<PageId>(before)}
            && f.recovery.lastRollbackStats().pagesTruncated == 0,
            "Implicit failed append leaked/truncated its reserved page");
        f.validate();
    }
}
void tcpLifetime() {
    for (bool crash : {false, true}) {
        test::TemporaryDatabase db("owned_tcp_lifetime");
        sql::SelectResult expected;
        {Storage f(db.path().string()); f.create(); expected = f.rows();}
        int commands[2], replies[2];
        require(::pipe(commands) == 0 && ::pipe(replies) == 0, "TCP test pipe failed");
        const auto child = ::fork();
        if (child == 0) {
            ::close(commands[1]); ::close(replies[0]); ::alarm(30);
            try {
                Storage f(db.path().string()); net::ServerConfig config; config.port = 0;
                net::TcpServer server(config, f.engine, f.pool, f.disk); server.start();
                const auto port = server.port();
                require(::write(replies[1], &port, sizeof(port)) == sizeof(port), "TCP port pipe failed");
                std::jthread serving([&] {try {server.serve(1);} catch (const std::exception& e) {
                    std::cerr << e.what() << '\n'; ::_exit(98);}});
                char signal = 0;
                require(::read(commands[0], &signal, 1) == 1, "TCP authorize signal missing");
                // The single accepted connection owns session 1. Its BEGIN
                // response has completed; hold its real session latch to bind.
                test::OwnedSqlTestAccess::bind(f.engine, 1);
                require(::write(replies[1], &signal, 1) == 1, "TCP authorize ACK missing");
                if (crash) {
                    require(::read(commands[0], &signal, 1) == 1, "TCP STEAL signal missing");
                    f.pool.flushAll(); f.log.flushAll(); f.disk.sync();
                    require(::write(replies[1], &signal, 1) == 1, "TCP STEAL ACK missing");
                    // Parent SIGKILLs a real server with live worker/transaction.
                    require(::read(commands[0], &signal, 1) == 1, "TCP crash wait interrupted");
                    ::_exit(99);
                }
                serving.join(); f.validate();
                require(f.rows().rows == expected.rows
                    && f.engine.transactionManager().stats().disconnectRollbacks == 1,
                    "TCP disconnect did not rollback owned SQL scope");
                f.pool.flushAll(); f.disk.sync(); server.close(); ::_exit(0);
            } catch (const std::exception& e) {std::cerr << "TCP child: " << e.what() << '\n'; ::_exit(98);}
        }
        require(child > 0, "TCP server fork failed");
        struct ChildGuard {
            pid_t pid;
            ~ChildGuard() {if (pid > 0) {::kill(pid, SIGKILL); int status; ::waitpid(pid, &status, 0);}}
        } guard{child};
        ::close(commands[0]); ::close(replies[1]);
        std::uint16_t port = 0;
        require(::read(replies[0], &port, sizeof(port)) == sizeof(port), "TCP server did not start");
        net::MiniDbClient client("127.0.0.1", port); client.connect(); client.handshake();
        static_cast<void>(client.execute("BEGIN"));
        char signal = 'x';
        require(::write(commands[1], &signal, 1) == 1 && ::read(replies[0], &signal, 1) == 1,
            "TCP private authorization failed");
        static_cast<void>(client.execute("UPDATE t SET value = 'uncommitted' WHERE id = 0"));
        static_cast<void>(client.execute("CREATE TABLE transient (id UINT32 PRIMARY KEY)"));
        static_cast<void>(client.execute("INSERT INTO transient VALUES (7)"));
        if (crash) {
            require(::write(commands[1], &signal, 1) == 1 && ::read(replies[0], &signal, 1) == 1,
                "TCP STEAL handshake failed");
            require(::kill(child, SIGKILL) == 0, "TCP server kill failed");
        }
        client.close();
        int status = 0; require(::waitpid(child, &status, 0) == child, "TCP server wait failed"); guard.pid = -1;
        require(crash ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
            : WIFEXITED(status) && WEXITSTATUS(status) == 0, "Unexpected TCP server completion");
        ::close(commands[1]); ::close(replies[0]);
        Storage f(db.path().string()); f.validate();
        require(f.rows().rows == expected.rows && f.rows().recordIds == expected.recordIds
            && !f.catalog.findTable("transient"), "TCP lifecycle restart lost committed contents/RIDs");
    }
}
void corruptNewestControl(const std::string& path) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    require(static_cast<bool>(file), "Missing checkpoint control");
    std::uint64_t generation = 0;
    std::size_t newest = 0;
    for (std::size_t index = 0; index < checkpoint_control_layout::SLOT_COUNT; ++index) {
        std::array<std::byte, checkpoint_slot_layout::SIZE> bytes{};
        file.seekg(checkpoint_control_layout::HEADER_SIZE + index * bytes.size());
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        require(static_cast<bool>(file), "Cannot read checkpoint slot");
        const auto slot = decodeCheckpointSlot(bytes);
        if (slot.generation > generation) {generation = slot.generation; newest = index;}
    }
    const auto offset = checkpoint_control_layout::HEADER_SIZE
        + newest * checkpoint_slot_layout::SIZE + checkpoint_slot_layout::CHECKSUM_OFFSET;
    file.seekg(offset); char byte = 0; file.read(&byte, 1); byte ^= 1;
    file.seekp(offset); file.write(&byte, 1); file.flush();
    require(static_cast<bool>(file), "Cannot corrupt newest control slot");
}
void crashAndRetention() {
    const char* points[] = {"owned_after_cleanup_begin_force", "owned_after_intent_force",
        "reclaim_after_wal_force", "reclaim_after_free_write", "reclaim_after_metadata_write",
        "reclaim_after_database_sync", "owned_after_progress_append", "owned_after_progress_force",
        "owned_after_abort_append", "owned_after_abort_force"};
    unsigned cases = 0;
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto store : {WalStorageMode::LegacySingleFile, WalStorageMode::Segmented})
       for (unsigned controlCase = 0; controlCase < 3; ++controlCase) for (const auto* point : points) {
        test::TemporaryDatabase db("owned_sql_crash");
        sql::SelectResult expected;
        PageId root = INVALID_PAGE_ID;
        std::vector<PageId> originalFree;
        std::uint64_t originalExtent = 0;
        {Storage f(db.path().string(), mode, store); f.create(); expected = f.rows();
         root = f.disk.databaseHeader().catalogRootPageId;
         originalFree = f.allocator.freePageIds(); originalExtent = f.disk.pageCount();
         static_cast<void>(f.checkpoints.checkpoint(CheckpointMode::Sharp));}
        const auto child = ::fork();
        if (child == 0) {
            try {Storage f(db.path().string(), mode, store);
                f.exec("BEGIN"); f.exec("UPDATE t SET value = 'changed' WHERE id = 0");
                f.exec("CREATE TABLE transient (id UINT32 PRIMARY KEY)");
                f.exec("INSERT INTO transient VALUES (8)");
                static_cast<void>(f.engine.transactionManager().checkpoint(LOCAL_SESSION_ID, CheckpointMode::Fuzzy));
                require(f.checkpoints.stats().attEntriesCaptured == 1
                    && f.checkpoints.stats().dptEntriesCaptured > 0, "Live fuzzy ATT/DPT not exercised");
                f.pool.flushAll(); ::setenv("MINIDB_FAILPOINT", point, 1);
                f.exec("ROLLBACK"); ::_exit(99);
            } catch (const std::exception& e) {std::cerr << point << ": " << e.what() << '\n'; ::_exit(98);}
        }
        int status = 0;
        require(child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status)
            && WEXITSTATUS(status) == 86, "SQL cleanup crash boundary not reached");
        if (controlCase == 1) require(std::filesystem::remove(db.path().string() + ".ckpt"), "Missing control file");
        if (controlCase == 2) corruptNewestControl(db.path().string() + ".ckpt");
        try {
            for (unsigned reopen = 0; reopen < 2; ++reopen) {
                Storage f(db.path().string(), mode, store);
                require(f.rows().rows == expected.rows && f.rows().recordIds == expected.recordIds,
                    "SQL restart lost original rows/RIDs");
                require(!f.catalog.findTable("transient") && f.disk.databaseHeader().catalogRootPageId == root,
                    "SQL restart retained uncommitted catalog entry");
                std::vector<PageId> expectedFree = originalFree;
                for (std::uint64_t page = originalExtent; page < f.disk.pageCount(); ++page)
                    expectedFree.insert(expectedFree.begin(), static_cast<PageId>(page));
                require(f.allocator.freePageIds() == expectedFree, "SQL restart lost/duplicated an owned page");
                f.validate();
                if (reopen == 1) {
                    // Completed cleanup may be checkpointed/reclaimed and then
                    // reused by real table creation, without stale CLR lineage.
                    static_cast<void>(f.checkpoints.checkpoint(CheckpointMode::Sharp));
                    f.exec("CREATE TABLE reused (id UINT32 PRIMARY KEY)");
                    f.exec("INSERT INTO reused VALUES (19)"); f.validate();
                }
            }
            Storage f(db.path().string(), mode, store); f.validate();
            require(f.catalog.openTable("reused").size() == 1 && f.rows().rows == expected.rows,
                "Post-cleanup checkpoint/reuse lost committed table");
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("SQL crash point=") + point + " control="
                + std::to_string(controlCase) + " mode=" + std::to_string(static_cast<int>(mode))
                + " store=" + std::to_string(static_cast<int>(store)) + ": " + e.what());
        }
        ++cases;
       }
    std::cout << "SQL cleanup crash/control cases=" << cases << '\n';
}
}
int main(int argc, char** argv) {
    try {if (argc == 2 && std::string_view(argv[1]) == "--crash") crashAndRetention();
        else if (argc == 2 && std::string_view(argv[1]) == "--tcp") tcpLifetime();
        else {directed(); implicitFailure();}
        std::cout << "Owned SQL/storage integration passed (test-only dispatch)\n";}
    catch (const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
