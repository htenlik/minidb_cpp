#include "minidb/database_metadata_manager.hpp"
#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/recovery.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using minidb::test::require;

void waitForExit(pid_t child, int expected, const std::string& description) {
    int status = 0;
    require(::waitpid(child, &status, 0) == child, "waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == expected,
            description + "; wait status=" + std::to_string(status));
}

void createThreeUpdateLoser(
    const std::string& path,
    minidb::WalUpdateMode mode = minidb::WalUpdateMode::Adaptive,
    bool segmented = false) {
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        minidb::DiskManager disk(path);
        const auto page1 = disk.appendPage();
        const auto page2 = disk.appendPage();
        const auto page3 = disk.appendPage();
        minidb::LogManager log(
            minidb::walPathForDatabase(path),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::EagerValidated,
            segmented ? minidb::WalStorageMode::Segmented
                      : minidb::WalStorageMode::LegacySingleFile,
            9000);
        minidb::RecoveryCoordinator coordinator(
            disk, log, minidb::INVALID_TRANSACTION_ID, mode);
        minidb::DatabaseMetadataManager metadata(disk, coordinator, log);
        coordinator.beginStatement();
        metadata.updateCatalogRootPageId(page1);
        metadata.updateFreeListRootPageId(page2);
        metadata.updateCatalogRootPageId(page3);
        disk.sync();
        ::_exit(91);
    }
    waitForExit(child, 91, "Loser setup child failed");
}

void crashRecoveryAt(
    const std::string& path,
    const char* failpoint,
    bool segmented = false) {
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        ::setenv("MINIDB_FAILPOINT", failpoint, 1);
        minidb::DiskManager disk(path);
        minidb::LogManager log(
            minidb::walPathForDatabase(path),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery,
            segmented ? minidb::WalStorageMode::Segmented
                      : minidb::WalStorageMode::LegacySingleFile,
            9000);
        static_cast<void>(minidb::RecoveryManager(disk, log).recover());
        ::_exit(92);
    }
    waitForExit(child, 86, std::string("Recovery missed failpoint ") + failpoint);
}

std::size_t countRecords(
    const std::string& path,
    minidb::LogRecordType type,
    bool segmented = false) {
    minidb::LogManager log(
        minidb::walPathForDatabase(path),
        minidb::LogManager::DEFAULT_BUFFER_SIZE,
        minidb::LogOpenMode::EagerValidated,
        segmented ? minidb::WalStorageMode::Segmented
                  : minidb::WalStorageMode::LegacySingleFile,
        9000);
    std::size_t count = 0;
    for (const auto& record : log.scan().records) {
        if (record.type == type) ++count;
    }
    return count;
}

void requireRolledBackPageZero(minidb::DiskManager& disk) {
    require(disk.databaseHeader().catalogRootPageId == minidb::INVALID_PAGE_ID
                && disk.databaseHeader().freeListRootPageId == minidb::INVALID_PAGE_ID,
            "Restartable UNDO did not restore page-0 metadata");
}

void testCrashAfterDurableClrResumesRemainingUndo() {
    minidb::test::TemporaryDatabase database("clr_resume_remaining");
    const auto path = database.path().string();
    createThreeUpdateLoser(path);
    crashRecoveryAt(path, "recovery_after_compensation_page_write");
    require(countRecords(path, minidb::LogRecordType::Compensation) == 1,
            "First interrupted recovery did not durably publish one CLR");

    minidb::DiskManager disk(path);
    minidb::LogManager log(
        minidb::walPathForDatabase(path),
        minidb::LogManager::DEFAULT_BUFFER_SIZE,
        minidb::LogOpenMode::DeferredRecovery);
    const auto stats = minidb::RecoveryManager(disk, log).recover();
    require(stats.undoRestartCount == 1
                && stats.undoClrsEncountered == 1
                && stats.undoRecordsSkippedByClr == 1
                && stats.undoUserRecordsVisited == 2
                && stats.undoUserRecordsCompensated == 2
                && stats.clrsAppended == 2,
            "Restart did not resume from the durable CLR undoNextLSN");
    require(countRecords(path, minidb::LogRecordType::Compensation) == 3
                && countRecords(path, minidb::LogRecordType::Abort) == 1,
            "Completed restart did not leave three CLRs followed by ABORT");
    requireRolledBackPageZero(disk);
}

void testRepeatedRecoveryCrashesConverge() {
    minidb::test::TemporaryDatabase database("clr_repeated_recovery_crashes");
    const auto path = database.path().string();
    createThreeUpdateLoser(path, minidb::WalUpdateMode::FullPage);
    for (std::size_t expected = 1; expected <= 3; ++expected) {
        crashRecoveryAt(path, "recovery_after_compensation_page_write");
        require(countRecords(path, minidb::LogRecordType::Compensation) == expected,
                "Repeated recovery crash did not advance exactly one durable CLR");
    }
    {
        minidb::DiskManager disk(path);
        minidb::LogManager log(
            minidb::walPathForDatabase(path),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.undoUserRecordsCompensated == 0
                    && stats.clrsAppended == 0
                    && stats.undoClrsEncountered == 1
                    && stats.abortedTransactions == 1,
                "Final restart repeated already-compensated user updates");
        requireRolledBackPageZero(disk);
    }
}

void testCrashBoundaryMatrix() {
    for (const auto* failpoint : {
             "recovery_before_clr_append",
             "recovery_after_clr_append",
             "recovery_after_clr_wal_force",
             "recovery_midway_loser_chain",
             "recovery_after_final_clr",
             "recovery_before_abort_append",
             "recovery_after_abort_append",
             "recovery_after_abort_fsync",
         }) {
        minidb::test::TemporaryDatabase database(
            std::string("clr_boundary_") + failpoint);
        const auto path = database.path().string();
        createThreeUpdateLoser(path, minidb::WalUpdateMode::ByteRange);
        crashRecoveryAt(path, failpoint);
        minidb::DiskManager disk(path);
        minidb::LogManager log(
            minidb::walPathForDatabase(path),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        static_cast<void>(minidb::RecoveryManager(disk, log).recover());
        requireRolledBackPageZero(disk);
        require(countRecords(path, minidb::LogRecordType::Abort) == 1,
                std::string("Crash boundary did not converge to one ABORT: ") + failpoint);
    }
}

void testAppendedPageTruncationIsRestartable() {
    minidb::test::TemporaryDatabase database("clr_truncation_restart");
    const auto path = database.path().string();
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        minidb::DiskManager disk(path);
        minidb::LogManager log(minidb::walPathForDatabase(path));
        minidb::RecoveryCoordinator coordinator(disk, log);
        minidb::BufferPoolManager pool(disk, 1, 2, &log, &coordinator);
        coordinator.attachBufferPool(pool);
        coordinator.beginStatement();
        auto page = pool.newPageWrite();
        const auto pageId = page->pageId();
        page->data()[100] = std::byte{0xA5};
        page->drop();
        require(pool.flushPage(pageId), "Could not persist appended loser page");
        disk.sync();
        ::_exit(91);
    }
    waitForExit(child, 91, "Appended-page loser setup failed");
    crashRecoveryAt(path, "recovery_after_appended_page_truncation");
    {
        minidb::DiskManager disk(path);
        require(disk.pageCount() == 1,
                "Interrupted recovery did not persist idempotent page truncation");
        minidb::LogManager log(
            minidb::walPathForDatabase(path),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        const auto stats = minidb::RecoveryManager(disk, log).recover();
        require(stats.clrsAppended == 0 && stats.pagesTruncated == 0
                    && disk.pageCount() == 1,
                "Restart did not safely repeat already-completed truncation");
    }
}

void testClrUndoAcrossSegmentBoundaries() {
    minidb::test::TemporaryDatabase database("clr_segment_boundaries");
    const auto path = database.path().string();
    createThreeUpdateLoser(path, minidb::WalUpdateMode::FullPage, true);
    crashRecoveryAt(path, "recovery_after_compensation_page_write", true);
    require(countRecords(path, minidb::LogRecordType::Compensation, true) == 1,
            "Segmented WAL did not retain the first forced CLR");
    minidb::DiskManager disk(path);
    minidb::LogManager log(
        minidb::walPathForDatabase(path),
        minidb::LogManager::DEFAULT_BUFFER_SIZE,
        minidb::LogOpenMode::DeferredRecovery,
        minidb::WalStorageMode::Segmented,
        9000);
    const auto stats = minidb::RecoveryManager(disk, log).recover();
    require(stats.undoRestartCount == 1 && stats.clrsAppended == 2
                && log.stats().retainedSegments >= 4,
            "Restartable UNDO did not traverse update/CLR links across WAL segments");
    requireRolledBackPageZero(disk);
}

} // namespace

int main() {
    try {
        testCrashAfterDurableClrResumesRemainingUndo();
        testRepeatedRecoveryCrashesConverge();
        testCrashBoundaryMatrix();
        testAppendedPageTruncationIsRestartable();
        testClrUndoAcrossSegmentBoundaries();
        std::cout << "restartable_undo_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "restartable_undo_test failed: " << error.what() << '\n';
        return 1;
    }
}
