#include "writeback_fixture.hpp"
#include "concurrency_utils.hpp"
#include "minidb/database_server.hpp"

#include <barrier>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace minidb;
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void run(unsigned kind, unsigned samples) {
    const unsigned count = kind == 2 ? 1000 : (kind == 1 || kind == 3 ? 100 : 1);
    test::TemporaryDatabase db("frame_retirement_benchmark");
    std::vector<double> times;
    std::uint64_t dbSync = 0, walSync = 0, writes = 0, retired = 0, renewed = 0, retirementNs = 0;
    std::uint64_t retained = 0, deleted = 0, hits = 0, requests = 0;
    {
        test::WritebackFixture f(db.path().string(), WalUpdateMode::Adaptive,
            WalStorageMode::Segmented, kind == 6 ? 2 : count + 3);
        f.initialize(count + 10);
        for (PageId id = 1; id <= count + 1; ++id) {
            DiskManager::Page bytes{};
            SlottedPageView::initialize(bytes, id, f.disk.pageCount(), count + 9);
            static_cast<void>(SlottedPageView(bytes, id, f.disk.pageCount()).insert(TupleBytes(32, std::byte{0})));
            f.disk.writePhysicalPage(id, bytes);
        }
        f.disk.sync();
        for (unsigned sample = 0; sample < samples; ++sample) {
            f.recovery.beginStatement();
            for (PageId id = 1; id <= count; ++id) {
                auto guard = requireWritePage(f.pool, id, "benchmark mutation");
                test::require(SlottedPageView(guard.data(), id, f.disk.pageCount()).tryUpdate(0,
                    TupleBytes(32, static_cast<std::byte>(sample + 1))), "Benchmark update failed");
            }
            f.recovery.commitStatement(); f.pool.flushAll();
            if (kind == 3) f.update(count + 1, sample + 1);
            if (kind == 6) for (PageId id = 2; id <= 4; ++id) { auto guard = requireReadPage(f.pool, id, "benchmark eviction"); }
            const auto before = f.pool.stats(); const auto diskBefore = f.disk.writebackStats(); const auto walBefore = f.log.stats();
            const auto start = Clock::now();
            static_cast<void>(f.checkpoint(kind == 5 ? CheckpointMode::Sharp : CheckpointMode::Fuzzy));
            times.push_back(milliseconds(start));
            const auto after = f.pool.stats();
            writes += after.physicalPageWrites - before.physicalPageWrites;
            dbSync += f.disk.writebackStats().databaseSyncsCompleted - diskBefore.databaseSyncsCompleted;
            walSync += f.log.stats().fsyncCalls - walBefore.fsyncCalls;
#ifndef MINIDB_RETIREMENT_BASELINE
            retired += after.cleanDependenciesRetired - before.cleanDependenciesRetired;
            retirementNs += after.retirementDurationNs - before.retirementDurationNs;
#endif
            test::require(after.physicalPageWrites == before.physicalPageWrites, "Retirement checkpoint wrote page bytes");
        }
        const auto stats = f.pool.stats(); hits = stats.cacheHits; requests = stats.pageRequests;
#ifndef MINIDB_RETIREMENT_BASELINE
        renewed = stats.dependenciesRenewedAfterRetirement;
        f.pool.validate();
#endif
        retained = f.log.stats().physicalWalBytes; deleted = f.log.stats().segmentsDeleted;
    }
    const auto start = Clock::now();
    { test::WritebackFixture f(db.path().string()); test::require(f.startup.checkpointUsed, "No recovered benchmark checkpoint"); }
    const auto recoveryMs = milliseconds(start);
    std::sort(times.begin(), times.end());
    auto percentile = [&](unsigned p) { return times[(times.size() * p + 99) / 100 - 1] * 1000; };
    constexpr const char* names[] = {"one-clean", "100-clean", "1000-clean", "mixed", "repeated-fuzzy", "repeated-sharp", "eviction"};
    std::cout << names[kind] << ',' << samples << ',' << percentile(50) << ',' << percentile(95) << ',' << percentile(99)
        << ',' << retirementNs / samples / 1000.0 << ',' << walSync << ',' << dbSync << ',' << writes
        << ',' << retired << ',' << renewed << ',' << retained << ',' << deleted
        << ',' << static_cast<double>(hits) / static_cast<double>(requests) << ',' << recoveryMs << '\n';
}
void sqlReaders(unsigned queries) {
    test::TemporaryDatabase db("frame_retirement_sql_readers"); net::ServerConfig config;
    config.port = 0; config.bufferFrames = 16; config.checkpointWalBytes = 0;
    net::DatabaseServer database(db.path().string(), config);
    auto& engine = database.sqlEngine();
    static_cast<void>(engine.execute("CREATE TABLE t (id UINT32 PRIMARY KEY, value UINT32)"));
    static_cast<void>(engine.execute("INSERT INTO t VALUES (1, 7)"));
    for (unsigned n = 0; n < 4; ++n) static_cast<void>(database.checkpointManager().checkpoint());
    const auto before = database.logManager().lastValidOffset();
    std::barrier start(8); test::ThreadErrors errors; std::vector<std::jthread> readers;
    const auto began = Clock::now();
    for (SessionId id = 1; id <= 8; ++id) readers.emplace_back([&, id] { errors.run([&] {
        static_cast<void>(engine.execute("BEGIN READ ONLY", id)); start.arrive_and_wait();
        for (unsigned n = 0; n < queries; ++n) {
            const auto result = engine.execute("SELECT value FROM t WHERE id = 1", id);
            test::require(std::get<std::uint32_t>(std::get<sql::SelectResult>(result).rows[0][0]) == 7, "SQL reader mismatch");
        }
        static_cast<void>(engine.execute("COMMIT", id)); engine.closeSession(id);
    }); });
    readers.clear(); errors.rethrow(); const auto ms = milliseconds(began);
    test::require(database.logManager().lastValidOffset() == before, "Readers produced transaction WAL");
    std::cout << "readonly_select_queries=" << queries * 8 << " queries_per_second=" << queries * 8000.0 / ms << '\n';
}
}
int main(int argc, char** argv) {
    try {
        const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke";
        std::cout << "case,samples,p50_us,p95_us,p99_us,retirement_mean_us,wal_fsyncs,db_fsyncs,checkpoint_writes,retired,renewed,retained_wal_bytes,deleted_segments,hit_ratio,recovery_ms\n";
        for (unsigned kind = 0; kind < 7; ++kind) run(kind, smoke ? 3 : 30);
        sqlReaders(smoke ? 20 : 1000);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
