#include "minidb/database_server.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/slotted_page.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace minidb;
using test::require;

void waitCrash(pid_t child) {
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Crash point was not reached; wait status=" + std::to_string(status));
}

void exactOrphanRegression() {
    test::TemporaryDatabase db("durable_append_orphan");
    const auto child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        DiskManager disk(db.path().string());
        LogManager log(walPathForDatabase(db.path().string()));
        RecoveryCoordinator recovery(disk, log);
        BufferPoolManager pool(disk, 2, 2, &log, &recovery);
        recovery.attachBufferPool(pool);
        recovery.beginStatement();
        { auto page = pool.newPageWrite(); if (!page) ::_exit(92); }
        ::_exit(86); // same reservation-before-initialization crash as the audit
    }
    waitCrash(child);
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
                   LogOpenMode::DeferredRecovery);
    require(disk.pageCount() == 2, "Orphan regression did not physically append P1");
    const auto input = log.scan();
    require(input.records.size() == 2 && input.records.front().type == LogRecordType::Begin
            && input.records.back().type == LogRecordType::PageReserve
            && decodeBeginLogPayload(input.records.front().payload).startPageCount == 1,
            "First append did not persist its original BEGIN boundary");
    const auto stats = RecoveryManager(disk, log).recover();
    require(stats.loserTransactions == 1 && stats.pagesTruncated == 1 && disk.pageCount() == 1,
            "Reservation crash left an unreachable physical orphan");
    BufferPoolManager pool(disk, 2, 2);
    PageAllocator allocator(pool, disk);
    allocator.validate();
    require(allocator.freePageIds().empty(), "Truncated P1 remains on free list");
    require(RecoveryManager(disk, log).recover().loserTransactions == 0, "Second restart repeated rollback");
}

void matrix() {
    const char* points[] = {
        "append_before_begin_append", "append_after_begin_before_force", "append_after_begin_force",
        "append_before_physical_extension", "append_after_physical_extension",
        "append_after_frame_installation", "zero_initialized", "before_page_update_append",
        "after_page_update_append", "before_commit_append", "after_commit_sync",
        "append_after_reservation_append", "append_after_reservation_force", "after_commit_append",
        "recovery_midway_loser_chain", "recovery_after_abort_sync",
    };
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
        for (bool segmented : {false, true}) {
            for (const auto* point : points) {
                test::TemporaryDatabase db("durable_append_matrix");
                { DiskManager disk(db.path().string()); static_cast<void>(disk.appendPage()); disk.sync(); }
                const auto child = ::fork();
                require(child >= 0, "fork failed");
                if (child == 0) {
                    ::setenv("MINIDB_FAILPOINT", point, 1);
                    DiskManager disk(db.path().string());
                    LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
                                   LogOpenMode::EagerValidated,
                                   segmented ? WalStorageMode::Segmented : WalStorageMode::LegacySingleFile, 9000);
                    RecoveryCoordinator recovery(disk, log, 1, mode);
                    BufferPoolManager pool(disk, 4, 2, &log, &recovery);
                    recovery.attachBufferPool(pool);
                    PageAllocator allocator(pool, disk);
                    recovery.beginStatement();
                    { auto page = pool.fetchPageWrite(1); page->data()[80] = std::byte{0x33}; }
                    const auto pageId = allocator.allocatePage(); // zero initialized by allocator
                    if (std::string_view(point) == "zero_initialized") ::_exit(86);
                    {
                        auto page = pool.fetchPageWrite(pageId);
                        SlottedPageView::initialize(page->data(), pageId, disk.pageCount(), 1);
                        SlottedPageView view(page->data(), pageId, disk.pageCount());
                        static_cast<void>(view.insert(TupleBytes(128, std::byte{0xB2})));
                    }
                    recovery.prepareStatement();
                    if (std::string_view(point).starts_with("recovery_")) recovery.rollbackStatement();
                    recovery.commitStatement();
                    ::_exit(92);
                }
                waitCrash(child);
                DiskManager disk(db.path().string());
                LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
                               LogOpenMode::DeferredRecovery,
                               segmented ? WalStorageMode::Segmented : WalStorageMode::LegacySingleFile, 9000);
                const bool committed = std::string_view(point) == "after_commit_sync";
                const auto durableHistory = log.scan();
                const auto reservationCount = static_cast<std::uint64_t>(std::count_if(
                    durableHistory.records.begin(), durableHistory.records.end(), [](const auto& r) {
                        return r.type == LogRecordType::PageReserve;
                    }));
                RecoveryManager manager(disk, log);
                const auto stats = manager.recover();
                require(stats.pageReservationsRecovered == reservationCount, "Crash analysis lost reservation provenance");
                if (reservationCount != 0) {
                    const auto& entry = manager.recoveredTransactions().front().reservations.at(2);
                    require(entry.expectedOldPageCount == 2 && !entry.materializedInRuntime
                        && log.readRecordAt(entry.reserveLsn).transactionId == 1,
                        "Recovered reservation fabricated volatile materialization or wrong owner");
                }
                require(disk.pageCount() == (committed ? 3U : 2U),
                        "Wrong recovered extent at " + std::string(point) + " mode="
                            + std::string(walUpdateModeName(mode)));
                if (committed) {
                    DiskManager::Page bytes{};
                    disk.readPage(2, bytes);
                    require(ConstSlottedPageView(bytes, 2, 3).get(0) == TupleBytes(128, std::byte{0xB2}),
                            "Durable commit lost initialized page");
                    require(stats.committedTransactions == 1, "Durable commit not classified as winner");
                }
                DiskManager::Page original{};
                disk.readPage(1, original);
                require(original[80] == (committed ? std::byte{0x33} : std::byte{0}),
                        "Mixed reservation/update CLR chain lost original page at " + std::string(point));
                BufferPoolManager pool(disk, 4, 2);
                PageAllocator allocator(pool, disk);
                allocator.validate();
                require(allocator.freePageIds().empty(), "Recovered extent/free list disagree");
                require(RecoveryManager(disk, log).recover().loserTransactions == 0,
                        "Repeated restart did not converge");
            }
        }
    }
    std::cout << "append/reservation crash matrix: 96 subprocess boundaries passed\n";
}

net::ServerConfig config() {
    net::ServerConfig settings;
    settings.port = 0; settings.bufferFrames = 32;
    settings.checkpointWalBytes = 0; settings.checkpointStatements = 0;
    settings.walUpdateMode = WalUpdateMode::Adaptive;
    return settings;
}

void sqlSteadyStateAndReadOnly() {
    test::TemporaryDatabase db("durable_append_sql_cycles");
    auto settings = config();
    {
        net::DatabaseServer server(db.path().string(), settings);
        auto& sql = server.sqlEngine();
        static_cast<void>(sql.execute("CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(64))"));
        const auto original = server.diskManager().pageCount();
        for (int cycle = 0; cycle < 100; ++cycle) {
            static_cast<void>(sql.execute("BEGIN"));
            static_cast<void>(sql.execute("INSERT INTO t VALUES (1, 'rollback')"));
            require(server.diskManager().pageCount() > original, "Cycle did not append heap/index pages");
            static_cast<void>(sql.execute("ROLLBACK"));
            require(server.diskManager().pageCount() == original, "Equivalent rollback workload grew extent");
            server.catalog().validate(); server.pageAllocator().validate(); server.bufferPool().validate();
            require(std::get<sql::SelectResult>(sql.execute("SELECT * FROM t")).rows.empty(), "Rollback left a row");
        }
        const auto count = server.logManager().scan().records.size();
        static_cast<void>(sql.execute("BEGIN READ ONLY"));
        static_cast<void>(sql.execute("SELECT * FROM t"));
        static_cast<void>(sql.execute("COMMIT"));
        require(server.logManager().scan().records.size() == count, "Read-only scope generated transaction WAL");
        const auto previous = count;
        static_cast<void>(sql.execute("BEGIN"));
        static_cast<void>(sql.execute("INSERT INTO t VALUES (1, 'committed')"));
        static_cast<void>(sql.execute("INSERT INTO t VALUES (2, 'also-committed')"));
        static_cast<void>(sql.execute("COMMIT"));
        const auto records = server.logManager().scan().records;
        require(std::count_if(records.begin() + static_cast<std::ptrdiff_t>(previous), records.end(), [](const auto& r) {
            return r.type == LogRecordType::Begin;
        }) == 1, "Explicit transaction with several inserts duplicated BEGIN");
    }
    net::DatabaseServer reopened(db.path().string(), settings);
    reopened.catalog().validate(); reopened.pageAllocator().validate();
    require(std::get<sql::SelectResult>(reopened.sqlEngine().execute("SELECT * FROM t")).rows.size() == 2,
            "Committed allocation structures did not survive reopen");
}
} // namespace

int main() {
    try {
        exactOrphanRegression(); matrix(); sqlSteadyStateAndReadOnly();
        std::cout << "durable_page_append_crash_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "durable_page_append_crash_test failed: " << error.what() << '\n';
        return 1;
    }
}
