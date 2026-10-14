#include "minidb/log_manager.hpp"
#include "minidb/database_server.hpp"
#include "minidb/recovery.hpp"
#include "test_utils.hpp"

#include <chrono>
#include <iostream>

namespace {
using namespace minidb;
using Clock = std::chrono::steady_clock;
std::uint64_t nsSince(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
std::uint64_t p95(std::vector<std::uint64_t> values) {
    std::sort(values.begin(), values.end());
    return values[(values.size() * 95 + 99) / 100 - 1];
}
std::uint64_t p99(std::vector<std::uint64_t> values) {
    std::sort(values.begin(), values.end());
    return values[(values.size() * 99 + 99) / 100 - 1];
}
void measure(WalUpdateMode mode, unsigned appendCount) {
    constexpr unsigned SAMPLES = 20;
    std::vector<std::uint64_t> mutations, allocations, commits;
    std::uint64_t elapsed = 0, totalWalBytes = 0;
    for (unsigned sample = 0; sample < SAMPLES; ++sample) {
        test::TemporaryDatabase db("append_force_cost");
        DiskManager disk(db.path().string());
        const auto existing = disk.appendPage();
        LogManager log(walPathForDatabase(db.path().string()));
        RecoveryCoordinator recovery(disk, log, 1, mode);
        BufferPoolManager pool(disk, 8, 2, &log, &recovery);
        recovery.attachBufferPool(pool);
        recovery.beginStatement();
        log.resetStats(); // exclude WAL header creation's fsync
        const auto start = Clock::now();
        if (appendCount == 0) {
            auto page = pool.fetchPageWrite(existing);
            page->data()[80] = std::byte{1};
        } else {
            for (unsigned i = 0; i < appendCount; ++i) {
                const auto allocationStart = Clock::now();
                auto page = pool.newPageWrite();
                allocations.push_back(nsSince(allocationStart));
                test::require(page.has_value(), "Allocation benchmark ran out of frames");
                page->data()[80] = static_cast<std::byte>(i + 1);
            }
        }
        mutations.push_back(nsSince(start));
        test::require(log.stats().fsyncCalls == appendCount,
                      "Unexpected pre-commit BEGIN force count");
        const auto commitStart = Clock::now();
        recovery.commitStatement();
        commits.push_back(nsSince(commitStart));
        elapsed += mutations.back() + commits.back();
        totalWalBytes += log.stats().bytesAppended;
        const auto records = log.scan().records;
        test::require(std::count_if(records.begin(), records.end(), [](const auto& r) {
                          return r.type == LogRecordType::Begin;
                      }) == 1, "Allocation benchmark duplicated BEGIN");
        test::require(log.stats().fsyncCalls == appendCount + 1,
                      "Unexpected transaction WAL fsync count");
        test::require(recovery.stats().pageReservationsLogged == appendCount
            && recovery.stats().reservationWalForces == appendCount
            && recovery.stats().reservationWalBytes == appendCount * 72U,
            "Reservation benchmark accounting mismatch");
    }
    std::cout << "mode=" << walUpdateModeName(mode) << " workload="
              << (appendCount == 0 ? "existing-update" : appendCount == 1 ? "first-append" : "four-appends")
              << " samples=" << SAMPLES << " begin_records=1 wal_fsyncs=" << appendCount + 1
              << " reserve_records=" << appendCount << " reservation_wal_bytes=" << appendCount * 72U
              << " mean_total_wal_bytes=" << totalWalBytes / SAMPLES
              << " transactions_per_second=" << (1.0e9 * SAMPLES / static_cast<double>(elapsed))
              << " mutation_or_allocation_p95_ns=" << p95(mutations)
              << " allocation_p95_ns=" << (allocations.empty() ? 0 : p95(allocations))
              << " allocation_p99_ns=" << (allocations.empty() ? 0 : p99(allocations))
              << " commit_p95_ns=" << p95(commits) << '\n';
}

void allocationHeavyInsert(WalUpdateMode mode) {
    constexpr unsigned SAMPLES = 20, ROWS = 16;
    std::vector<std::uint64_t> inserts, commits;
    std::uint64_t elapsed = 0, reserves = 0, forces = 0, reservationBytes = 0, totalBytes = 0, begins = 0;
    for (unsigned sample = 0; sample < SAMPLES; ++sample) {
        test::TemporaryDatabase db("reservation_insert_cost");
        net::ServerConfig config; config.port = 0; config.bufferFrames = 64; config.walUpdateMode = mode;
        net::DatabaseServer server(db.path().string(), config);
        auto exec = [&](const std::string& text) {
            static_cast<void>(server.sqlEngine().execute(text)); // SQL errors throw.
        };
        exec("CREATE TABLE data (id UINT32 PRIMARY KEY, value VARCHAR(3200))");
        server.logManager().resetStats(); server.recoveryCoordinator().resetStats();
        const auto walStart = server.logManager().lastValidOffset();
        exec("BEGIN");
        const auto start = Clock::now();
        for (unsigned i = 0; i < ROWS; ++i) {
            const auto insertStart = Clock::now();
            exec("INSERT INTO data VALUES (" + std::to_string(i) + ", '" + std::string(3000, 'x') + "')");
            inserts.push_back(nsSince(insertStart));
        }
        const auto commitStart = Clock::now(); exec("COMMIT"); commits.push_back(nsSince(commitStart));
        elapsed += nsSince(start);
        const auto statistics = server.recoveryCoordinator().stats();
        reserves += statistics.pageReservationsLogged; reservationBytes += statistics.reservationWalBytes;
        forces += server.logManager().stats().fsyncCalls; totalBytes += server.logManager().stats().bytesAppended;
        const auto records = server.logManager().scanFrom(walStart).records;
        begins += static_cast<std::uint64_t>(std::count_if(records.begin(), records.end(), [](const auto& r) { return r.type == LogRecordType::Begin; }));
        test::require(statistics.pageReservationsLogged >= ROWS
            && statistics.reservationWalBytes == statistics.pageReservationsLogged * 72U,
            "INSERT workload did not exercise allocation/accounting");
        server.catalog().validate(); server.pageAllocator().validate();
    }
    test::require(begins == SAMPLES, "INSERT benchmark duplicated BEGIN");
    std::cout << "mode=" << walUpdateModeName(mode) << " workload=allocation-heavy-insert samples=" << SAMPLES
        << " rows_per_transaction=" << ROWS << " begin_records=" << begins / SAMPLES
        << " reserve_records=" << reserves / SAMPLES << " wal_fsyncs=" << forces / SAMPLES
        << " reservation_wal_bytes=" << reservationBytes / SAMPLES << " mean_total_wal_bytes=" << totalBytes / SAMPLES
        << " rows_per_second=" << (1.0e9 * SAMPLES * ROWS / static_cast<double>(elapsed))
        << " allocating_insert_p95_ns=" << p95(inserts) << " allocating_insert_p99_ns=" << p99(inserts)
        << " commit_p95_ns=" << p95(commits) << '\n';
}
} // namespace
int main() {
    try {
        for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
            for (unsigned count : {0U, 1U, 4U}) measure(mode, count);
            allocationHeavyInsert(mode);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "durable_page_append_benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
