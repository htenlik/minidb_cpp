#include "writeback_fixture.hpp"

#include <chrono>
#include <iostream>

namespace {
using namespace minidb;
using Clock = std::chrono::steady_clock;
std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
void run(unsigned kind, unsigned samples) {
    test::TemporaryDatabase db("frontier_benchmark");
    std::vector<std::uint64_t> times;
    std::uint64_t dbSyncs = 0, walSyncs = 0, writes = 0, distance = 0, dpt = 0, retained = 0, deleted = 0;
    {
        test::WritebackFixture f(db.path().string(), WalUpdateMode::Adaptive, WalStorageMode::Segmented, kind == 3 ? 1 : 128);
        f.initialize(128);
        for (PageId id = 1; id <= 100; ++id) {
            DiskManager::Page page{};
            SlottedPageView::initialize(page, id, f.disk.pageCount(), 127);
            static_cast<void>(SlottedPageView(page, id, f.disk.pageCount()).insert(TupleBytes(32, std::byte{0})));
            f.disk.writePhysicalPage(id, page);
        }
        f.disk.writePhysicalPage(126, reclaimedPage({126, INVALID_PAGE_ID, 128}, INVALID_LSN));
        f.disk.sync();
        for (unsigned n = 0; n < samples; ++n) {
            if (kind == 1 || kind == 3 || kind == 5) {
                f.update(1, 1 + n % 255);
                if (kind != 3) test::require(f.pool.flushPage(1), "Benchmark flush failed");
            }
            if (kind == 2) {
                f.recovery.beginStatement();
                for (PageId id = 1; id <= 100; ++id) {
                    auto guard = requireWritePage(f.pool, id, "100 pending benchmark writes");
                    test::require(SlottedPageView(guard.data(), id, 128).tryUpdate(0,
                        TupleBytes(32, static_cast<std::byte>(1 + n % 255))), "Benchmark replacement failed");
                }
                f.recovery.commitStatement(); f.pool.flushAll();
            }
            if (kind == 3) {
                auto guard = requireReadPage(f.pool, 2, "benchmark eviction");
                test::require(!f.pool.isResident(1), "Pending benchmark page was not evicted");
            }
            if (kind == 4) f.root(n % 2 ? INVALID_PAGE_ID : 126);
            if (kind == 5) f.update(2, 1 + n % 255);
            const auto before = f.disk.writebackStats();
            const auto bufferBefore = f.pool.stats();
            const auto walBefore = f.log.stats();
            const auto dirty = f.pool.dirtyPageTableSnapshot();
            distance += before.physicalWriteEpoch - before.synchronizedWriteEpoch;
            const auto start = Clock::now(); static_cast<void>(f.checkpoint()); times.push_back(elapsed(start));
            dbSyncs += f.disk.writebackStats().databaseSyncsCompleted - before.databaseSyncsCompleted;
            walSyncs += f.log.stats().fsyncCalls - walBefore.fsyncCalls;
            writes += f.pool.stats().physicalPageWrites - bufferBefore.physicalPageWrites;
            dpt += dirty.size();
            test::require(f.pool.dirtyPageTableSnapshot() == dirty
                && f.pool.stats().physicalPageWrites == bufferBefore.physicalPageWrites, "Benchmark fence flushed dirty buffers");
        }
        retained = f.log.stats().physicalWalBytes; deleted = f.log.stats().segmentsDeleted;
    }
    const auto recoveryStart = Clock::now();
    { test::WritebackFixture f(db.path().string()); test::require(f.startup.checkpointUsed, "Benchmark did not recover checkpoint"); }
    const auto recoveryNs = elapsed(recoveryStart);
    std::sort(times.begin(), times.end());
    const auto percentile = [&](unsigned p) { return times[(times.size() * p + 99) / 100 - 1] / 1000.0; };
    static constexpr const char* names[] = {"no-pending", "one-clean", "100-writebacks", "evicted", "page-zero", "mixed-dirty"};
    std::cout << names[kind] << ',' << samples << ',' << percentile(50) << ',' << percentile(95) << ',' << percentile(99)
              << ',' << dbSyncs << ',' << walSyncs << ',' << writes << ',' << distance / samples
              << ',' << dpt / samples << ',' << retained << ',' << deleted << ',' << recoveryNs / 1e6 << '\n';
}
}
int main(int argc, char** argv) {
    try {
        const unsigned samples = argc == 2 && std::string_view(argv[1]) == "--smoke" ? 3 : 30;
        std::cout << "case,samples,p50_us,p95_us,p99_us,db_fsyncs,wal_fsyncs,checkpoint_db_writes,mean_pending_epochs,mean_dpt,retained_wal_bytes,segments_deleted,recovery_ms\n";
        for (unsigned kind = 0; kind < 6; ++kind) run(kind, samples);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
