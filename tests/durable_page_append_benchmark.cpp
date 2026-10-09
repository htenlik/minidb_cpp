#include "minidb/log_manager.hpp"
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
void measure(WalUpdateMode mode, unsigned appendCount) {
    constexpr unsigned SAMPLES = 20;
    std::vector<std::uint64_t> mutations, commits;
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
                auto page = pool.newPageWrite();
                test::require(page.has_value(), "Allocation benchmark ran out of frames");
                page->data()[80] = static_cast<std::byte>(i + 1);
            }
        }
        mutations.push_back(nsSince(start));
        test::require(log.stats().fsyncCalls == (appendCount == 0 ? 0U : 1U),
                      "Unexpected pre-commit BEGIN force count");
        const auto commitStart = Clock::now();
        recovery.commitStatement();
        commits.push_back(nsSince(commitStart));
        const auto records = log.scan().records;
        test::require(std::count_if(records.begin(), records.end(), [](const auto& r) {
                          return r.type == LogRecordType::Begin;
                      }) == 1, "Allocation benchmark duplicated BEGIN");
        test::require(log.stats().fsyncCalls == (appendCount == 0 ? 1U : 2U),
                      "Unexpected transaction WAL fsync count");
    }
    std::cout << "mode=" << walUpdateModeName(mode) << " workload="
              << (appendCount == 0 ? "existing-update" : appendCount == 1 ? "first-append" : "four-appends")
              << " samples=" << SAMPLES << " begin_records=1 wal_fsyncs=" << (appendCount == 0 ? 1 : 2)
              << " mutation_or_allocation_p95_ns=" << p95(mutations)
              << " commit_p95_ns=" << p95(commits) << '\n';
}
} // namespace
int main() {
    try {
        for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
            for (unsigned count : {0U, 1U, 4U}) measure(mode, count);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "durable_page_append_benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
