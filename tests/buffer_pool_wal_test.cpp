#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using minidb::test::require;

class FakeWal final : public minidb::WalFlushProvider {
public:
    std::unordered_set<minidb::Lsn> known;
    minidb::Lsn durable = minidb::INVALID_LSN;
    bool fail = false;
    std::vector<minidb::Lsn> flushes;
    std::function<void(minidb::Lsn)> beforeFlush;

    [[nodiscard]] minidb::Lsn durableLsn() const noexcept override { return durable; }
    [[nodiscard]] bool containsLsn(minidb::Lsn lsn) const noexcept override {
        return known.contains(lsn);
    }
    void flushUpTo(minidb::Lsn lsn) override {
        flushes.push_back(lsn);
        if (beforeFlush) beforeFlush(lsn);
        if (fail) throw std::runtime_error("injected WAL durability failure");
        durable = lsn;
    }
};

std::vector<minidb::PageId> appendPages(minidb::DiskManager& disk, std::size_t count) {
    std::vector<minidb::PageId> result;
    for (std::size_t index = 0; index < count; ++index) result.push_back(disk.appendPage());
    return result;
}

std::byte diskByte(minidb::DiskManager& disk, minidb::PageId pageId, std::size_t offset) {
    minidb::DiskManager::Page page{};
    disk.readPage(pageId, page);
    return page[offset];
}

void testPageLsnApiAndMonotonicity() {
    minidb::test::TemporaryDatabase database("wal_page_lsn_api");
    minidb::DiskManager disk(database.path().string());
    const auto pageId = disk.appendPage();
    FakeWal wal;
    wal.known = {64, 128};
    minidb::BufferPoolManager pool(disk, 1, 2, &wal);
    auto page = pool.fetchPageWrite(pageId);
    require(page.has_value() && page->pageLsn() == minidb::INVALID_LSN,
            "Freshly loaded frame did not start with INVALID_LSN");
    page->setPageLsn(64);
    page->setPageLsn(64);
    page->setPageLsn(128);
    require(page->pageLsn() == 128 && pool.pageLsn(pageId) == 128,
            "Write guard did not associate the newest pageLSN");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { page->setPageLsn(64); }, "pageLSN regression was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { page->setPageLsn(minidb::INVALID_LSN); },
        "Write guard accepted INVALID_LSN assignment");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { page->setPageLsn(999); }, "Write guard accepted an unknown LSN");
}

void testWalFailurePreventsFlushAndEviction() {
    minidb::test::TemporaryDatabase database("wal_failure_blocks_page");
    minidb::DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 2);
    FakeWal wal;
    wal.known = {64};
    wal.fail = true;
    minidb::BufferPoolManager pool(disk, 1, 2, &wal);
    {
        auto page = pool.fetchPageWrite(pages[0]);
        page->data()[77] = std::byte{0xD7};
        page->setPageLsn(64);
    }
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(pool.flushPage(pages[0])); },
        "flushPage ignored injected WAL durability failure");
    require(diskByte(disk, pages[0], 77) == std::byte{0}
                && pool.isResident(pages[0]) && pool.isDirty(pages[0]) == true
                && pool.pageLsn(pages[0]) == 64,
            "Failed WAL flush wrote, unmapped, cleaned, or changed the dirty page");
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(pool.fetchPageRead(pages[1])); },
        "Dirty eviction ignored injected WAL durability failure");
    require(pool.isResident(pages[0]) && !pool.isResident(pages[1])
                && pool.stats().physicalPageWrites == 0
                && pool.stats().evictions == 0,
            "Failed WAL eviction reused the protected frame");
    {
        const auto page = pool.fetchPageRead(pages[0]);
        require(page->data()[77] == std::byte{0xD7} && page->pageLsn() == 64,
                "Failed WAL eviction did not preserve bytes/pageLSN");
    }
    pool.validate();
}

void testDirtyEvictionOrdersWalBeforePageAndResetsLsn() {
    minidb::test::TemporaryDatabase database("wal_dirty_eviction");
    minidb::DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 2);
    FakeWal wal;
    wal.known = {100};
    wal.beforeFlush = [&](minidb::Lsn lsn) {
        require(lsn == 100 && diskByte(disk, pages[0], 91) == std::byte{0},
                "Database page was written before its WAL flush callback");
    };
    minidb::BufferPoolManager pool(disk, 1, 2, &wal);
    {
        auto page = pool.fetchPageWrite(pages[0]);
        page->data()[91] = std::byte{0x91};
        page->setPageLsn(100);
    }
    {
        const auto replacement = pool.fetchPageRead(pages[1]);
        require(replacement.has_value() && replacement->pageLsn() == minidb::INVALID_LSN,
                "Victim pageLSN leaked into reused frame");
    }
    require(wal.flushes == std::vector<minidb::Lsn>{100}
                && diskByte(disk, pages[0], 91) == std::byte{0x91},
            "Dirty eviction did not flush WAL then persist page A");
    require(pool.stats().walFlushRequests == 1
                && pool.stats().dirtyEvictions == 1
                && pool.stats().physicalPageWrites == 1,
            "Dirty WAL eviction statistics are incorrect");
    {
        const auto reloaded = pool.fetchPageRead(pages[0]);
        require(reloaded->data()[91] == std::byte{0x91}
                    && reloaded->pageLsn() == minidb::INVALID_LSN,
                "Reloaded 11A page incorrectly retained volatile pageLSN");
    }
}

void testCleanEvictionDoesNotForceWal() {
    minidb::test::TemporaryDatabase database("wal_clean_eviction");
    minidb::DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 2);
    FakeWal wal;
    wal.known = {200};
    minidb::BufferPoolManager pool(disk, 1, 2, &wal);
    {
        auto page = pool.fetchPageWrite(pages[0]);
        page->data()[1] = std::byte{1};
        page->setPageLsn(200);
    }
    require(pool.flushPage(pages[0]), "Clean-eviction setup flush failed");
    wal.flushes.clear();
    {
        const auto page = pool.fetchPageRead(pages[1]);
        require(page.has_value(), "Clean page could not be evicted");
    }
    require(wal.flushes.empty(), "Clean page eviction performed a meaningless WAL flush");
}

void testFlushAllForcesMaximumOnce() {
    minidb::test::TemporaryDatabase database("wal_flush_all_max");
    minidb::DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 3);
    FakeWal wal;
    wal.known = {100, 250, 400};
    minidb::BufferPoolManager pool(disk, 3, 2, &wal);
    const std::array<minidb::Lsn, 3> lsns{100, 400, 250};
    for (std::size_t index = 0; index < pages.size(); ++index) {
        auto page = pool.fetchPageWrite(pages[index]);
        page->data()[10] = static_cast<std::byte>(index + 1);
        page->setPageLsn(lsns[index]);
    }
    wal.beforeFlush = [&](minidb::Lsn lsn) {
        require(lsn == 400, "flushAll did not request maximum resident pageLSN");
        for (const auto pageId : pages) {
            require(diskByte(disk, pageId, 10) == std::byte{0},
                    "flushAll wrote a database page before the maximum WAL LSN was durable");
        }
    };
    pool.flushAll();
    require(wal.flushes == std::vector<minidb::Lsn>{400}
                && pool.stats().walFlushRequests == 1
                && pool.stats().physicalPageWrites == 3,
            "flushAll did not coalesce WAL forcing to one maximum-LSN request");
    for (std::size_t index = 0; index < pages.size(); ++index) {
        require(diskByte(disk, pages[index], 10) == static_cast<std::byte>(index + 1),
                "flushAll did not persist a logged dirty page");
    }
}

void testActualLogManagerIntegrationAndNewPageLsn() {
    minidb::test::TemporaryDatabase database("wal_real_integration");
    minidb::DiskManager disk(database.path().string());
    minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
    minidb::BufferPoolManager pool(disk, 1, 2, &log);
    auto page = pool.newPageWrite();
    require(page->pageLsn() == minidb::INVALID_LSN,
            "Newly appended page did not start with INVALID_LSN");
    const auto pageId = page->pageId();
    minidb::LogRecord record;
    record.type = minidb::LogRecordType::PageUpdate;
    record.transactionId = 1;
    record.payload = {std::byte{0xA1}};
    const auto lsn = log.append(std::move(record));
    page->data()[5] = std::byte{0xA1};
    page->setPageLsn(lsn);
    page->drop();
    require(pool.flushPage(pageId) && log.durableLsn() >= lsn
                && diskByte(disk, pageId, 5) == std::byte{0xA1},
            "Real LogManager did not enforce WAL durability before page flush");
}

} // namespace

int main() {
    try {
        testPageLsnApiAndMonotonicity();
        testWalFailurePreventsFlushAndEviction();
        testDirtyEvictionOrdersWalBeforePageAndResetsLsn();
        testCleanEvictionDoesNotForceWal();
        testFlushAllForcesMaximumOnce();
        testActualLogManagerIntegrationAndNewPageLsn();
        std::cout << "buffer_pool_wal_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "buffer_pool_wal_test failed: " << error.what() << '\n';
        return 1;
    }
}
