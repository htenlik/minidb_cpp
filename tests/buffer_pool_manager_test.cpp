#include "minidb/buffer_pool_manager.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using minidb::BufferPoolManager;
using minidb::DiskManager;
using minidb::PageId;
using minidb::test::require;

static_assert(!std::is_copy_constructible_v<minidb::BasicPageGuard>);
static_assert(!std::is_copy_constructible_v<minidb::ReadPageGuard>);
static_assert(!std::is_copy_constructible_v<minidb::WritePageGuard>);
static_assert(std::is_move_constructible_v<minidb::ReadPageGuard>);
static_assert(std::is_same_v<
              decltype(std::declval<const minidb::ReadPageGuard&>().data()),
              std::span<const std::byte, minidb::database_format::PAGE_SIZE>>);

std::vector<PageId> appendPages(DiskManager& disk, std::size_t count) {
    std::vector<PageId> pages;
    for (std::size_t index = 0; index < count; ++index) {
        const auto pageId = disk.appendPage();
        DiskManager::Page page{};
        page[0] = static_cast<std::byte>(pageId & 0xFFU);
        page[1] = static_cast<std::byte>((pageId * 3U) & 0xFFU);
        disk.writePage(pageId, page);
        pages.push_back(pageId);
    }
    return pages;
}

void testConstructionFetchAndCapacity() {
    minidb::test::TemporaryDatabase database("buffer_basic");
    DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 3);
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { BufferPoolManager invalid(disk, 0); }, "zero-frame buffer pool was accepted");

    BufferPoolManager pool(disk, 1, 2);
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(pool.fetchPageRead(0)); },
        "buffer pool exposed metadata page through read fetch");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(pool.fetchPageWrite(0)); },
        "buffer pool exposed metadata page through write fetch");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(pool.fetchPageRead(minidb::INVALID_PAGE_ID)); },
        "buffer pool accepted invalid PageId");
    {
        auto first = pool.fetchPageRead(pages[0]);
        require(first.has_value() && first->data()[0] == std::byte{1},
                "first buffer fetch did not read page");
        require(pool.stats().cacheMisses == 1 && pool.stats().physicalPageReads == 1,
                "first fetch metrics were incorrect");
        auto blocked = pool.fetchPageRead(pages[1]);
        require(!blocked.has_value(), "capacity-one pool evicted a pinned page");
    }
    {
        auto hit = pool.fetchPageRead(pages[0]);
        require(hit.has_value() && pool.stats().cacheHits == 1,
                "second same-page fetch was not a hit");
    }
    const auto frameA = pool.frameIdForPage(pages[0]);
    {
        auto other = pool.fetchPageRead(pages[1]);
        require(other.has_value() && !pool.isResident(pages[0])
                    && pool.isResident(pages[1]) && pool.residentPageCount() == 1,
                "clean eviction or capacity bound failed");
        require(pool.frameIdForPage(pages[1]) == frameA,
                "frame was not safely reused for a different PageId");
    }
    const auto stats = pool.stats();
    require(stats.evictions == 1 && stats.dirtyEvictions == 0
                && stats.physicalPageWrites == 0 && stats.capacity == 1,
            "clean eviction statistics were incorrect");
    pool.validate();
}

void testGuardMovesPinsAndDirtyState() {
    minidb::test::TemporaryDatabase database("buffer_guards");
    DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 3);
    BufferPoolManager pool(disk, 2, 2);

    auto firstOptional = pool.fetchPageRead(pages[0]);
    auto first = std::move(*firstOptional);
    require(!firstOptional->isValid() && pool.pinCount(pages[0]) == 1,
            "moving guard did not transfer its pin");
    auto second = pool.fetchPageRead(pages[0]);
    require(second.has_value() && pool.pinCount(pages[0]) == 2,
            "two guards did not own two pins");
    second->drop();
    second->drop();
    require(pool.pinCount(pages[0]) == 1 && pool.stats().evictableFrames == 0,
            "explicit guard drop was not idempotent or released too many pins");

    auto other = pool.fetchPageRead(pages[1]);
    require(other.has_value() && first.data()[0] == std::byte{1},
            "guarded page became invalid while its pin remained alive");
    minidb::ReadPageGuard moved(std::move(first));
    require(!first.isValid() && moved.isValid(), "guard move construction failed");
    auto assignmentTarget = std::move(*other);
    assignmentTarget = std::move(moved);
    require(!moved.isValid() && assignmentTarget.pageId() == pages[0]
                && pool.pinCount(pages[1]) == 0 && pool.pinCount(pages[0]) == 1,
            "guard move assignment did not release/transfer pins exactly once");
    assignmentTarget.drop();
    require(pool.pinCount(pages[0]) == 0 && pool.stats().evictableFrames == 2,
            "final guard release did not make frame evictable");

    {
        auto read = pool.fetchPageRead(pages[0]);
        require(read.has_value() && pool.isDirty(pages[0]) == false,
                "read guard dirtied a clean page");
    }
    {
        auto write = pool.fetchPageWrite(pages[0]);
        require(write.has_value() && pool.isDirty(pages[0]) == true,
                "write guard acquisition did not mark page dirty");
        write->data()[5] = std::byte{0xCC};
    }
    pool.validate();
}

void testDirtyVictimAndReopen() {
    minidb::test::TemporaryDatabase database("buffer_dirty_victim");
    PageId pageA = 0;
    PageId pageB = 0;
    {
        DiskManager disk(database.path().string());
        const auto pages = appendPages(disk, 2);
        pageA = pages[0];
        pageB = pages[1];
        BufferPoolManager pool(disk, 1, 2);
        {
            auto write = pool.fetchPageWrite(pageA);
            require(write.has_value(), "dirty-victim setup fetch failed");
            write->data()[100] = std::byte{0xD1};
            write->data()[4095] = std::byte{0xD2};
        }
        {
            auto readB = pool.fetchPageRead(pageB);
            require(readB.has_value(), "dirty victim prevented replacement fetch");
        }
        const auto stats = pool.stats();
        require(stats.evictions == 1 && stats.dirtyEvictions == 1
                    && stats.physicalPageWrites == 1,
                "dirty eviction counters were incorrect");
        pool.validate();
    }
    {
        DiskManager disk(database.path().string());
        BufferPoolManager pool(disk, 1, 2);
        auto read = pool.fetchPageRead(pageA);
        require(read.has_value() && read->data()[100] == std::byte{0xD1}
                    && read->data()[4095] == std::byte{0xD2},
                "dirty victim bytes did not persist across reopen");
    }
}

void testPinPressure() {
    minidb::test::TemporaryDatabase database("buffer_pin_pressure");
    DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 3);
    BufferPoolManager pool(disk, 2, 2);
    auto a = pool.fetchPageRead(pages[0]);
    auto b = pool.fetchPageRead(pages[1]);
    require(a.has_value() && b.has_value(), "pin-pressure setup failed");
    require(!pool.fetchPageRead(pages[2]).has_value(),
            "all-pinned buffer did not return NoFrameAvailable");
    const auto pageCount = disk.pageCount();
    require(!pool.newPageWrite().has_value() && disk.pageCount() == pageCount,
            "all-pinned newPageWrite appended a page without an available frame");
    a->drop();
    auto c = pool.fetchPageRead(pages[2]);
    require(c.has_value() && pool.isResident(pages[1]) && pool.pinCount(pages[1]) == 1,
            "released frame was not reused or pinned peer was evicted");
    pool.validate();
}

void testDestructorAttemptsDirtyFlush() {
    minidb::test::TemporaryDatabase database("buffer_destructor_flush");
    PageId pageId = minidb::INVALID_PAGE_ID;
    {
        DiskManager disk(database.path().string());
        pageId = disk.appendPage();
        {
            BufferPoolManager pool(disk, 1);
            auto page = pool.fetchPageWrite(pageId);
            require(page.has_value(), "destructor-flush setup fetch failed");
            page->data()[222] = std::byte{0x9A};
        }
        DiskManager::Page persisted{};
        disk.readPage(pageId, persisted);
        require(persisted[222] == std::byte{0x9A},
                "BufferPoolManager destructor did not attempt dirty flush");
    }
}

void testNewPageFlushesAndReset() {
    minidb::test::TemporaryDatabase database("buffer_new_flush");
    PageId pageId = minidb::INVALID_PAGE_ID;
    {
        DiskManager disk(database.path().string());
        BufferPoolManager pool(disk, 2, 3);
        auto page = pool.newPageWrite();
        require(page.has_value() && page->pageId() == 1,
                "newPageWrite did not append first data page");
        pageId = page->pageId();
        require(std::all_of(page->data().begin(), page->data().end(),
                            [](std::byte value) { return value == std::byte{0}; }),
                "new page was not zero initialized");
        page->data()[17] = std::byte{0x71};
        const auto nonresidentPageId = disk.appendPage();
        require(pool.flushPage(pageId), "flushPage rejected resident pinned page");
        require(pool.pinCount(pageId) == 1 && pool.isDirty(pageId) == false,
                "flushPage unpinned page or left it dirty");
        const auto writes = pool.stats().physicalPageWrites;
        require(pool.flushPage(pageId) && pool.stats().physicalPageWrites == writes,
                "flushing clean page performed physical write");
        require(!pool.flushPage(nonresidentPageId),
                "flushPage loaded or accepted nonresident page");
        page->data()[18] = std::byte{0x72};
        pool.flushAll();
        require(pool.pinCount(pageId) == 1 && pool.isDirty(pageId) == false,
                "flushAll evicted/unpinned frame or left it dirty");
        page->drop();
        pool.resetStats();
        const auto reset = pool.stats();
        require(reset.pageRequests == 0 && reset.physicalPageWrites == 0
                    && reset.residentPages == 1 && reset.evictableFrames == 1
                    && reset.capacity == 2,
                "resetStats cleared state gauges or retained event counters");
        pool.validate();
    }
    {
        DiskManager disk(database.path().string());
        BufferPoolManager pool(disk, 1);
        auto page = pool.fetchPageRead(pageId);
        require(page.has_value() && page->data()[17] == std::byte{0x71}
                    && page->data()[18] == std::byte{0x72},
                "explicit flush data did not survive reopen");
    }
}

void testExactLRU2VictimThroughPool() {
    minidb::test::TemporaryDatabase database("buffer_lru2_trace");
    DiskManager disk(database.path().string());
    const auto pages = appendPages(disk, 4);
    BufferPoolManager pool(disk, 3, 2);
    {
        auto guard = pool.fetchPageRead(pages[0]);
        require(guard.has_value(), "LRU-2 trace first fetch failed");
    }
    {
        auto guard = pool.fetchPageRead(pages[1]);
        require(guard.has_value(), "LRU-2 trace second fetch failed");
    }
    {
        auto guard = pool.fetchPageRead(pages[0]); // reused, finite LRU-2 history
        require(guard.has_value(), "LRU-2 trace hot fetch failed");
    }
    {
        auto guard = pool.fetchPageRead(pages[2]);
        require(guard.has_value(), "LRU-2 trace third-page fetch failed");
    }
    const auto victimFrame = pool.frameIdForPage(pages[1]);
    {
        auto guard = pool.fetchPageRead(pages[3]);
        require(guard.has_value(), "LRU-2 trace replacement fetch failed");
    }
    require(pool.isResident(pages[0]) && !pool.isResident(pages[1])
                && pool.isResident(pages[2]) && pool.isResident(pages[3]),
            "buffer pool did not choose exact LRU-2 infinity victim");
    require(pool.frameIdForPage(pages[3]) == victimFrame,
            "LRU-2 victim frame was not reused for new PageId");
    pool.validate();
}

} // namespace

int main() {
    try {
        testConstructionFetchAndCapacity();
        testGuardMovesPinsAndDirtyState();
        testDirtyVictimAndReopen();
        testPinPressure();
        testNewPageFlushesAndReset();
        testExactLRU2VictimThroughPool();
        testDestructorAttemptsDirtyFlush();
        std::cout << "BufferPoolManager and page-guard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BufferPoolManager test failure: " << error.what() << '\n';
        return 1;
    }
}
