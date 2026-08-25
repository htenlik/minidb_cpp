#include "minidb/log_manager.hpp"
#include "minidb/segmented_wal.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using minidb::test::require;

void createLegacy(const std::string& path) {
    minidb::LogManager log(path, 64);
    minidb::Lsn previous = minidb::INVALID_LSN;
    for (std::uint64_t id = 1; id <= 30; ++id) {
        previous = log.append(minidb::LogRecord{
            minidb::LogRecordType::PageUpdate,
            id,
            previous,
            std::vector<std::byte>(17, static_cast<std::byte>(id)),
        });
    }
    log.flushAll();
}

[[noreturn]] void migrateChild(const std::string& path, const char* failpoint) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::LogManager log(path, 64, minidb::LogOpenMode::DeferredRecovery,
                           minidb::WalStorageMode::Auto, 256);
    const auto scan = log.scan();
    log.completeRecoveryScan(scan);
    log.migrateLegacyToSegmented();
    ::_exit(90);
}

void expectMigrationCrash(const char* failpoint) {
    minidb::test::TemporaryDatabase database(
        std::string("wal_migration_") + failpoint);
    const auto legacy = minidb::walPathForDatabase(database.path().string());
    createLegacy(legacy);
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) migrateChild(legacy, failpoint);
    int status = 0;
    if (::waitpid(child, &status, 0) != child) throw std::runtime_error("waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            std::string("Migration child missed failpoint ") + failpoint);

    minidb::LogManager reopened(
        legacy, 64, minidb::LogOpenMode::EagerValidated,
        minidb::WalStorageMode::Auto, 256);
    const auto records = reopened.scan().records;
    require(records.size() == 30, "Crash-safe migration lost or duplicated WAL records");
    for (std::size_t index = 0; index < records.size(); ++index) {
        require(records[index].transactionId == index + 1,
                "Crash-safe migration changed logical WAL order");
    }
    require(reopened.isSegmented()
                == std::filesystem::exists(minidb::segmentedWalPathForLegacyWal(legacy)),
            "Startup did not prefer a durably published segmented WAL");
}

void testMigrationCrashMatrix() {
    for (const auto* failpoint : {
             "wal_migration_after_temp_create",
             "wal_migration_after_first_record",
             "wal_migration_after_final_record",
             "wal_migration_after_temp_sync",
             "wal_migration_after_rename",
             "wal_migration_after_parent_sync",
             "wal_migration_before_legacy_delete",
             "wal_migration_after_legacy_delete",
             "wal_migration_after_legacy_parent_sync",
         }) {
        expectMigrationCrash(failpoint);
    }
}

void testReclamationCrashMatrix() {
    for (const auto* failpoint : {
             "wal_reclaim_after_manifest",
             "wal_reclaim_after_unlink",
         }) {
        minidb::test::TemporaryDatabase database(
            std::string("wal_reclaim_") + failpoint);
        const auto legacy = minidb::walPathForDatabase(database.path().string());
        minidb::Lsn floor = minidb::INVALID_LSN;
        {
            minidb::LogManager log(
                legacy, 64, minidb::LogOpenMode::EagerValidated,
                minidb::WalStorageMode::Segmented, 256);
            minidb::Lsn previous = minidb::INVALID_LSN;
            for (std::uint64_t id = 1; id <= 80; ++id) {
                previous = log.append(minidb::LogRecord{
                    minidb::LogRecordType::PageUpdate, id, previous,
                    std::vector<std::byte>(31, static_cast<std::byte>(id)),
                });
                if (id == 60) floor = previous;
            }
            log.flushAll();
            log.rotateSegment();
        }
        const auto child = ::fork();
        if (child < 0) throw std::runtime_error("fork failed");
        if (child == 0) {
            ::setenv("MINIDB_FAILPOINT", failpoint, 1);
            minidb::LogManager log(
                legacy, 64, minidb::LogOpenMode::EagerValidated,
                minidb::WalStorageMode::Segmented, 256);
            static_cast<void>(log.reclaimSegmentsBefore(floor));
            ::_exit(90);
        }
        int status = 0;
        static_cast<void>(::waitpid(child, &status, 0));
        require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
                "Reclamation child missed failpoint");
        minidb::LogManager reopened(
            legacy, 64, minidb::LogOpenMode::EagerValidated,
            minidb::WalStorageMode::Segmented, 256);
        reopened.validate();
        require(!reopened.scan().records.empty(),
                "Reclamation crash removed the required retained WAL suffix");
    }
}

} // namespace

int main() {
    try {
        testMigrationCrashMatrix();
        testReclamationCrashMatrix();
        std::cout << "wal_migration_crash_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wal_migration_crash_test failed: " << error.what() << '\n';
        return 1;
    }
}
