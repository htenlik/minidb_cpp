#include "minidb/buffer_pool_manager.hpp"
#include "minidb/catalog.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/database_server.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/recovery.hpp"
#include "minidb/sql_executor.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {

using minidb::test::require;

void withLegacyEngine(
    const std::string& path,
    const std::function<void(minidb::sql::SqlEngine&)>& operation) {
    minidb::DiskManager disk(path);
    minidb::LogManager log(minidb::walPathForDatabase(path), 1);
    const auto startup = minidb::RecoveryManager(disk, log).recover();
    minidb::RecoveryCoordinator recovery(disk, log, startup.nextTransactionId);
    minidb::BufferPoolManager buffer(disk, 4, 2, &log, &recovery);
    recovery.attachBufferPool(buffer);
    minidb::DatabaseMetadataManager metadata(disk, recovery, log);
    minidb::PageAllocator allocator(buffer, disk, &metadata);
    std::optional<minidb::Catalog> catalog;
    if (disk.databaseHeader().catalogRootPageId == minidb::INVALID_PAGE_ID) {
        recovery.beginStatement();
        catalog.emplace(minidb::Catalog::openOrCreate(buffer, disk, allocator));
        recovery.commitStatement();
    } else {
        catalog.emplace(minidb::Catalog::open(buffer, disk, allocator));
    }
    minidb::sql::SqlEngine engine(*catalog, &recovery);
    operation(engine);
    catalog->validate();
}

std::size_t rows(minidb::net::DatabaseServer& server) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM legacy_items")).rows.size();
}

void createLegacyDatabase(const std::string& path) {
    withLegacyEngine(path, [](minidb::sql::SqlEngine& engine) {
        static_cast<void>(engine.execute(
            "CREATE TABLE legacy_items (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
        static_cast<void>(engine.execute("INSERT INTO legacy_items VALUES (1, 'committed')"));
    });
    require(std::filesystem::exists(minidb::walPathForDatabase(path))
                && !std::filesystem::exists(minidb::segmentedWalPathForDatabase(path)),
            "Legacy SQL setup did not create only a v1 monolithic WAL");
}

void testCommittedLegacyMigration() {
    minidb::test::TemporaryDatabase database("legacy_sql_migration");
    const auto path = database.path().string();
    createLegacyDatabase(path);
    minidb::Lsn migratedHighWater = minidb::INVALID_LSN;
    minidb::TransactionId nextTransaction = 0;
    {
        minidb::net::DatabaseServer server(path, {});
        require(rows(server) == 1 && server.logManager().isSegmented(),
                "Production startup did not recover/migrate committed legacy SQL state");
        require(!std::filesystem::exists(minidb::walPathForDatabase(path))
                    && std::filesystem::exists(minidb::segmentedWalPathForDatabase(path)),
                "Published migration did not retire the legacy WAL");
        require(server.logManager().oldestRetainedLsn()
                    > minidb::wal_file_layout::HEADER_SIZE,
                "Production migration retained obsolete pre-checkpoint WAL history");
        migratedHighWater = server.logManager().lastValidOffset();
        nextTransaction = server.recoveryCoordinator().nextTransactionId();
        server.catalog().validate();
        server.pageAllocator().validate();
    }
    {
        minidb::net::DatabaseServer reopened(path, {});
        static_cast<void>(reopened.sqlEngine().execute(
            "INSERT INTO legacy_items VALUES (2, 'after-migration')"));
        require(reopened.logManager().lastAppendedLsn() >= migratedHighWater
                    && reopened.recoveryCoordinator().nextTransactionId() > nextTransaction
                    && rows(reopened) == 2,
                "Post-migration LSN/TransactionId/data continuity failed");
    }
}

[[noreturn]] void legacyLoserChild(const std::string& path) {
    ::setenv("MINIDB_FAILPOINT", "before_commit_append", 1);
    withLegacyEngine(path, [](minidb::sql::SqlEngine& engine) {
        static_cast<void>(engine.execute(
            "INSERT INTO legacy_items VALUES (2, 'loser')"));
    });
    ::_exit(90);
}

void testLegacyLoserRecoveryBeforeMigration() {
    minidb::test::TemporaryDatabase database("legacy_loser_migration");
    const auto path = database.path().string();
    createLegacyDatabase(path);
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) legacyLoserChild(path);
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Legacy loser child missed pre-COMMIT failpoint");

    minidb::net::DatabaseServer migrated(path, {});
    require(migrated.startupRecoveryStats().loserTransactions == 1
                && rows(migrated) == 1 && migrated.logManager().isSegmented(),
            "Legacy loser was not undone before segmented migration");
    require(!std::filesystem::exists(minidb::walPathForDatabase(path)),
            "Legacy loser migration left obsolete monolithic WAL authoritative");
    migrated.catalog().validate();
    migrated.pageAllocator().validate();
}

} // namespace

int main() {
    try {
        testCommittedLegacyMigration();
        testLegacyLoserRecoveryBeforeMigration();
        std::cout << "legacy_wal_migration_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "legacy_wal_migration_test failed: " << error.what() << '\n';
        return 1;
    }
}
