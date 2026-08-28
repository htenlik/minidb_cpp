#include "minidb/byte_codec.hpp"
#include "minidb/database_server.hpp"
#include "minidb/page_lsn.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using minidb::test::require;

minidb::net::ServerConfig config() {
    return {"127.0.0.1", 0, 8, 16, 2, 0, 0};
}

void createLegacyDatabase(const std::string& path) {
    minidb::DiskManager disk(path);
    minidb::DiskManager::Page page{};
    disk.readPhysicalPage(0, page);
    minidb::byte_codec::writeUint32(
        page, minidb::database_format::FORMAT_VERSION_OFFSET,
        minidb::database_format::LEGACY_VERSION);
    minidb::clearPersistentPageLsn(page);
    disk.writePhysicalPage(0, page);
    disk.sync();
}

[[noreturn]] void migrationChild(const std::string& path, const char* failpoint) {
    ::setenv("MINIDB_FAILPOINT", failpoint, 1);
    minidb::net::DatabaseServer server(path, config());
    ::_exit(90);
}

void testMigrationCrashMatrix() {
    for (const auto* failpoint : {
             "migration_before_initial_checkpoint",
             "migration_after_initial_checkpoint",
             "migration_before_format_update",
             "migration_after_format_wal_append",
             "migration_before_commit_sync",
             "migration_after_commit_sync",
             "migration_before_final_checkpoint",
             "checkpoint_mid_control_write",
         }) {
        minidb::test::TemporaryDatabase database(
            std::string("page_lsn_migration_") + failpoint);
        createLegacyDatabase(database.path().string());
        const auto child = ::fork();
        if (child < 0) throw std::runtime_error("fork failed");
        if (child == 0) migrationChild(database.path().string(), failpoint);
        int status = 0;
        if (::waitpid(child, &status, 0) != child) {
            throw std::runtime_error("waitpid failed");
        }
        require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
                std::string("Migration child missed failpoint ") + failpoint);

        minidb::net::DatabaseServer recovered(database.path().string(), config());
        require(recovered.diskManager().databaseHeader().formatVersion
                    == minidb::database_format::CURRENT_VERSION,
                std::string("Migration did not converge after ") + failpoint);
        recovered.catalog().validate();
        recovered.pageAllocator().validate();
    }
}

} // namespace

int main() {
    try {
        testMigrationCrashMatrix();
        std::cout << "database format v1-to-v2 migration crash tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "database format migration crash test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
