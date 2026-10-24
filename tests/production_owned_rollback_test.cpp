#include "minidb/database_server.hpp"
#include "minidb/owned_rollback_log.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

// Deliberately no owned_rollback_fixture, friend access, protocol knob or
// alternative SQL dispatcher: use only the ordinary public server/engine.
namespace {
using namespace minidb;
using test::require;
using net::DatabaseServer;
net::ServerConfig config(WalUpdateMode mode) {
    net::ServerConfig result; result.port = 0; result.bufferFrames = 4;
    result.checkpointWalBytes = 0; result.walSegmentBytes = 9000; result.walUpdateMode = mode;
    return result;
}
void exec(DatabaseServer& s, const std::string& sql) {static_cast<void>(s.sqlEngine().execute(sql));}
sql::SelectResult rows(DatabaseServer& s) {return std::get<sql::SelectResult>(s.sqlEngine().execute("SELECT * FROM t"));}
void validate(DatabaseServer& s) {
    s.catalog().validate(); s.pageAllocator().validate(); s.bufferPool().validate();
    for (const auto& t : s.catalog().listTables()) s.catalog().openTable(t.tableId).validate();
}
void create(DatabaseServer& s) {
    const auto before = s.logManager().lastValidOffset();
    exec(s, "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
    const auto history = s.logManager().scanFrom(before).records;
    require(history.front().type == LogRecordType::BeginOwned, "Default SQL CREATE did not emit BEGIN_OWNED");
    exec(s, "BEGIN READ WRITE");
    const auto id = s.sqlEngine().transactionManager().activeTransactionId();
    const auto start = s.diskManager().pageCount();
    for (unsigned i = 0; i < 80; ++i) exec(s, "INSERT INTO t VALUES (" + std::to_string(i) + ", '" + std::string(60, 'v') + "')");
    exec(s, "COMMIT");
    unsigned begins = 0;
    for (const auto& r : s.logManager().scan().records) if (r.transactionId == id) {
        require(r.type != LogRecordType::Begin, "Explicit SQL emitted legacy BEGIN");
        if (r.type == LogRecordType::BeginOwned) {++begins; require(decodeOwnedBegin(r).startPageCount == start, "Transaction boundary changed");}
    }
    require(begins == 1, "Multi-statement writer did not retain exactly one owned BEGIN");
    validate(s);
}
void directed() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
        test::TemporaryDatabase db("production_owned"); sql::SelectResult expected;
        {DatabaseServer s(db.path().string(), config(mode)); create(s); expected = rows(s);
         const auto root = s.diskManager().databaseHeader().catalogRootPageId;
         const auto walBefore = s.logManager().lastValidOffset();
         exec(s, "BEGIN READ ONLY"); static_cast<void>(rows(s)); exec(s, "COMMIT");
         exec(s, "BEGIN"); static_cast<void>(rows(s)); exec(s, "ROLLBACK");
         require(s.logManager().lastValidOffset() == walBefore, "Zero-write/read-only scope generated WAL");
         for (unsigned scenario = 0; scenario < 5; ++scenario) {
            const auto extent = s.diskManager().pageCount(); const auto oldFree = s.pageAllocator().freePageIds();
            exec(s, "BEGIN");
            if (scenario == 0) for (unsigned i = 80; i < 520; ++i) exec(s, "INSERT INTO t VALUES (" + std::to_string(i) + ", 'new')");
            if (scenario == 1) exec(s, "UPDATE t SET value = '" + std::string(3500, 'X') + "' WHERE id = 0");
            if (scenario == 2) exec(s, "DELETE FROM t");
            if (scenario == 3) {exec(s, "CREATE TABLE transient (id UINT32 PRIMARY KEY)"); exec(s, "INSERT INTO transient VALUES (1)");}
            if (scenario == 4) {exec(s, "INSERT INTO t VALUES (900, 'pending')"); exec(s, "UPDATE t SET id = 1000 WHERE id = 1");}
            s.bufferPool().flushAll(); const auto materialized = s.diskManager().pageCount();
            exec(s, "ROLLBACK");
            require(rows(s).rows == expected.rows && rows(s).recordIds == expected.recordIds, "Production rollback lost rows/RIDs");
            auto free = oldFree;
            for (auto page = extent; page < materialized; ++page) free.insert(free.begin(), static_cast<PageId>(page));
            require(s.diskManager().pageCount() == materialized && s.pageAllocator().freePageIds() == free
                && s.recoveryCoordinator().lastRollbackStats().pagesTruncated == 0, "Production cleanup truncated/leaked/duplicated pages");
            require(s.diskManager().databaseHeader().catalogRootPageId == root && !s.catalog().findTable("transient"), "Catalog rollback failed");
            DiskManager::Page physical{}; s.diskManager().readPhysicalPage(root, physical);
            require(isValidLsn(readPersistentPageLsn(physical)), "Catalog PageLSN identity missing"); validate(s);
         }
         exec(s, "BEGIN"); exec(s, "INSERT INTO t VALUES (900, 'pending')");
         test::requireThrows<sql::SqlExecutionError>([&]{exec(s, "INSERT INTO t VALUES (0, 'duplicate')");}, "Mutating error not rejected");
         require(rows(s).rows == expected.rows && !s.sqlEngine().transactionManager().hasActiveExplicitTransaction(), "Failed statement did not abort scope");
         exec(s, "INSERT INTO t VALUES (999, 'committed')"); expected = rows(s); validate(s);}
        {DatabaseServer s(db.path().string(), config(mode)); validate(s);
         require(rows(s).rows == expected.rows && rows(s).recordIds == expected.recordIds, "Owned commit/reopen lost contents");}
    }
}
void poisonedCleanup() {
    test::TemporaryDatabase db("production_owned_failure");
    {DatabaseServer s(db.path().string(), config(WalUpdateMode::Adaptive));
     exec(s, "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
     exec(s, "BEGIN"); exec(s, "INSERT INTO t VALUES (1, 'pending')");
     ::setenv("MINIDB_THROWPOINT", "owned_after_intent_force", 1);
     test::requireThrows<sql::SqlExecutionError>([&]{exec(s, "ROLLBACK");}, "Cleanup failpoint did not stop rollback");
     ::unsetenv("MINIDB_THROWPOINT");
     require(s.sqlEngine().transactionManager().accessGate().stats().writerActive, "Failed cleanup released writer lease");
     test::requireThrows<sql::SqlExecutionError>([&]{static_cast<void>(rows(s));}, "Failed engine resumed reads");
     test::requireThrows<sql::SqlExecutionError>([&]{exec(s, "BEGIN READ ONLY");}, "Failed engine resumed admission");
     test::requireThrows<std::runtime_error>([&]{s.close();}, "Failed engine shutdown acknowledged unresolved cleanup");}
    DatabaseServer recovered(db.path().string(), config(WalUpdateMode::Adaptive));
    require(rows(recovered).rows.empty(), "Poisoned cleanup did not restart"); validate(recovered);
}
void implicitAppendFailure() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
        test::TemporaryDatabase db("production_owned_implicit_failure");
        DatabaseServer s(db.path().string(), config(mode));
        exec(s, "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
        const auto before = s.diskManager().pageCount();
        ::setenv("MINIDB_THROWPOINT", "append_after_physical_extension", 1);
        test::requireThrows<sql::SqlExecutionError>([&]{exec(s, "INSERT INTO t VALUES (1, 'pending')");}, "Implicit append failure was not rejected");
        ::unsetenv("MINIDB_THROWPOINT");
        require(rows(s).rows.empty() && !s.recoveryCoordinator().hasActiveStatement()
            && s.diskManager().pageCount() == before + 1
            && s.pageAllocator().freePageIds() == std::vector<PageId>{static_cast<PageId>(before)}
            && s.recoveryCoordinator().lastRollbackStats().pagesTruncated == 0, "Implicit owned failure leaked/truncated its page");
        validate(s);
    }
}
void internalStructure() {
    test::TemporaryDatabase db("production_owned_internal_structure");
    DatabaseServer s(db.path().string(), config(WalUpdateMode::Adaptive));
    exec(s, "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)");
    const auto meta = s.catalog().findTable("t")->primaryIndexMetadataPageId;
    exec(s, "BEGIN");
    {auto page = requireWritePage(s.bufferPool(), meta, "empty index test capacities");
     byte_codec::writeUint32(page.data(), persistent_index_metadata_layout::LEAF_MAX_KEYS_OFFSET, 3);
     byte_codec::writeUint32(page.data(), persistent_index_metadata_layout::INTERNAL_MAX_KEYS_OFFSET, 3);}
    for (unsigned i = 0; i < 40; ++i) exec(s, "INSERT INTO t VALUES (" + std::to_string(i) + ", 'v')");
    exec(s, "COMMIT"); const auto expected = rows(s);
    const auto index = PersistentBPlusTree::open(s.bufferPool(), s.diskManager(), s.pageAllocator(), meta);
    const auto root = index.rootPageId(); PageId firstChild = INVALID_PAGE_ID;
    {auto page = requireReadPage(s.bufferPool(), root, "internal root");
     firstChild = byte_codec::readUint32(page.data(), persistent_bplus_internal_layout::FIRST_CHILD_OFFSET);}
    {auto page = requireReadPage(s.bufferPool(), firstChild, "internal child");
     require(std::equal(persistent_bplus_internal_layout::MAGIC.begin(), persistent_bplus_internal_layout::MAGIC.end(), page.data().begin()),
        "Structural test did not reach three levels/internal splits");}
    exec(s, "BEGIN"); exec(s, "DELETE FROM t"); require(index.empty(), "Delete did not shrink root to empty");
    exec(s, "ROLLBACK"); validate(s);
    require(rows(s).rows == expected.rows && rows(s).recordIds == expected.recordIds && index.rootPageId() == root,
        "Production internal merge/root shrink rollback failed");
}
void crashes() {
    const char* points[] = {"owned_after_cleanup_begin_force", "owned_after_intent_force",
        "reclaim_after_wal_force", "reclaim_after_free_write", "reclaim_after_metadata_write",
        "reclaim_after_database_sync", "owned_after_progress_force", "owned_after_abort_append", "owned_after_abort_force"};
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) for (const auto* point : points) {
        test::TemporaryDatabase db("production_owned_crash"); sql::SelectResult expected;
        std::uint64_t before = 0; std::vector<PageId> originalFree;
        {DatabaseServer s(db.path().string(), config(mode)); create(s); expected = rows(s);
         before = s.diskManager().pageCount(); originalFree = s.pageAllocator().freePageIds();
         static_cast<void>(s.checkpointManager().checkpoint(CheckpointMode::Sharp));}
        const auto child = ::fork();
        if (child == 0) {try {DatabaseServer s(db.path().string(), config(mode));
            exec(s, "BEGIN"); exec(s, "UPDATE t SET value = 'loser' WHERE id = 0");
            exec(s, "CREATE TABLE transient (id UINT32 PRIMARY KEY)"); exec(s, "INSERT INTO transient VALUES (1)");
            static_cast<void>(s.sqlEngine().transactionManager().checkpoint(LOCAL_SESSION_ID, CheckpointMode::Fuzzy));
            s.bufferPool().flushAll(); ::setenv("MINIDB_FAILPOINT", point, 1); exec(s, "ROLLBACK"); ::_exit(99);
        } catch (const std::exception& e) {std::cerr << point << ": " << e.what() << '\n'; ::_exit(98);}}
        int status = 0; require(child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status)
            && WEXITSTATUS(status) == 86, "Production hard crash boundary not reached");
        if (std::string_view(point) == "owned_after_intent_force") {
            const auto restarting = ::fork();
            if (restarting == 0) {::setenv("MINIDB_FAILPOINT", "owned_tail_after_reclaim_force", 1);
                try {DatabaseServer s(db.path().string(), config(mode)); ::_exit(99);} catch (...) {::_exit(98);}}
            require(restarting > 0 && ::waitpid(restarting, &status, 0) == restarting
                && WIFEXITED(status) && WEXITSTATUS(status) == 86, "Production recovery interruption missed");
        }
        for (unsigned n = 0; n < 2; ++n) {DatabaseServer s(db.path().string(), config(mode)); validate(s);
            require(rows(s).rows == expected.rows && rows(s).recordIds == expected.recordIds
                && !s.catalog().findTable("transient"), "Production restart lost committed state");
            auto free = originalFree;
            for (auto page = before; page < s.diskManager().pageCount(); ++page) free.insert(free.begin(), static_cast<PageId>(page));
            require(s.pageAllocator().freePageIds() == free && s.startupRecoveryStats().pagesTruncated == 0,
                "Production restart truncated/leaked/duplicated pages");}
    }
}
}
int main(int argc, char** argv) {
    try {if (argc == 2 && std::string_view(argv[1]) == "--crash") crashes(); else {directed(); internalStructure(); implicitAppendFailure(); poisonedCleanup();}
        std::cout << "Default production owned rollback gate passed\n";
    } catch (const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
