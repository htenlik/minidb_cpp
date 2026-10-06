#include "minidb/buffer_pool_manager.hpp"
#include "concurrency_utils.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <iostream>
#include <latch>
#include <random>
#include <thread>
#include <vector>

using minidb::test::require;
using minidb::test::await;

void duplicateLoadAndLatches() {
    minidb::test::TemporaryDatabase db("buffer_concurrent_load");
    minidb::DiskManager disk(db.path().string());
    const auto page = disk.appendPage();
    minidb::BufferPoolManager pool(disk, 1);
    std::barrier start(8);
    std::latch ready(8), release(1);
    minidb::test::ThreadErrors errors;
    std::vector<std::jthread> readers;
    for (int i = 0; i < 8; ++i) readers.emplace_back([&] { errors.run([&] {
        start.arrive_and_wait();
        auto guard = pool.fetchPageRead(page);
        require(guard.has_value() && guard->data()[0] == std::byte{0}, "Simultaneous load failed");
        ready.count_down(); release.wait();
    }); });
    ready.wait();
    require(pool.stats().physicalPageReads == 1 && pool.residentPageCount() == 1
        && pool.pinCount(page) == 8, "Concurrent miss created duplicate frames/pins");
    pool.validate(); release.count_down();
    for (auto& thread : readers) thread.join();
    errors.rethrow();
    auto a = pool.fetchPageRead(page), b = pool.fetchPageRead(page);
    std::latch writerReady(1), releaseWriter(1);
    std::atomic<bool> writerEntered{false}, readerEntered{false};
    std::jthread writer([&] { errors.run([&] {
        auto guard = pool.fetchPageWrite(page);
        writerEntered = true; guard->data()[77] = std::byte{0x77};
        writerReady.count_down(); releaseWriter.wait();
    }); });
    await([&] { return pool.stats().contentLatchWaits == 1; }, "Write guard did not wait for shared owners");
    require(!writerEntered, "Writer overlapped frame readers");
    a->drop(); require(!writerEntered, "Dropping one reader released another reader's latch");
    b->drop(); writerReady.wait();
    std::jthread reader([&] { errors.run([&] {
        auto guard = pool.fetchPageRead(page);
        require(guard->data()[77] == std::byte{0x77}, "Reader saw partial writer content");
        readerEntered = true;
    }); });
    await([&] { return pool.stats().contentLatchWaits == 2; }, "Read guard did not wait for exclusive owner");
    require(!readerEntered, "Reader overlapped frame writer");
    releaseWriter.count_down(); writer.join(); reader.join(); errors.rethrow();
    pool.validate(); require(pool.totalPinCount() == 0, "Contention leaked frame pins");
}

void model(std::uint64_t seed) {
    minidb::test::TemporaryDatabase db("buffer_concurrent_model");
    minidb::DiskManager disk(db.path().string());
    std::vector<minidb::DiskManager::Page> expected(32);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto page = disk.appendPage();
        for (std::size_t offset = 0; offset < expected[i].size(); ++offset) {
            expected[i][offset] = static_cast<std::byte>((i * 17 + offset) % 256);
        }
        disk.writePage(page, expected[i]);
    }
    minidb::BufferPoolManager pool(disk, 4, 2, nullptr, nullptr, true);
    minidb::test::ThreadErrors errors;
    std::barrier start(8);
    std::vector<std::jthread> workers;
    for (std::uint64_t worker = 0; worker < 8; ++worker) workers.emplace_back([&, worker] { errors.run([&] {
        std::mt19937_64 random(seed + worker);
        start.arrive_and_wait();
        for (std::size_t op = 0; op < 4000; ++op) {
            const auto index = static_cast<std::size_t>(random() % expected.size());
            auto guard = pool.fetchPageRead(static_cast<minidb::PageId>(index + 1));
            require(guard.has_value() && std::equal(guard->data().begin(), guard->data().end(), expected[index].begin()),
                "seed=" + std::to_string(seed) + " thread=" + std::to_string(worker)
                    + " operation=" + std::to_string(op) + ": immutable page contents changed");
            if (op % 127 == 0) pool.validate();
        }
    }); });
    for (auto& worker : workers) worker.join();
    errors.rethrow(); pool.validate();
    require(pool.stats().evictions > 0 && pool.totalPinCount() == 0, "Concurrent model did not evict or leaked pins");
    // DiskManager's shared stream is also exercised directly by concurrent readers.
    workers.clear();
    for (std::size_t worker = 0; worker < 8; ++worker) workers.emplace_back([&, worker] { errors.run([&] {
        for (std::size_t op = 0; op < 128; ++op) {
            const auto index = (worker + op) % expected.size();
            minidb::DiskManager::Page copy{};
            disk.readPage(static_cast<minidb::PageId>(index + 1), copy);
            require(copy == expected[index], "Concurrent DiskManager seek/read race");
        }
    }); });
    for (auto& worker : workers) worker.join();
    errors.rethrow();
}
int main() {
    try {
        duplicateLoadAndLatches();
        for (auto seed : {0x12B10001ULL, 0x12B10002ULL, 0x12B10003ULL, 0x12B10004ULL}) model(seed);
        std::cout << "buffer_concurrency_test passed (128000 fetch/read/unpin operations, 8 threads, "
                     "4 frames; seeds 0x12B10001..0x12B10004)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
