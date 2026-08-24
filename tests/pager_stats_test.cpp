#include "minidb/pager.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

using minidb::Pager;
using minidb::PagerStats;

void requireZeroCounters(const PagerStats& stats, std::uint64_t residentPages) {
    minidb::test::require(
        stats.pageRequests == 0
            && stats.cacheHits == 0
            && stats.cacheMisses == 0
            && stats.physicalPageReads == 0
            && stats.physicalPageWrites == 0
            && stats.dirtyMarks == 0
            && stats.flushCalls == 0
            && stats.appendedPages == 0
            && stats.residentPages == residentPages,
        "Pager counters were not reset while preserving resident-page gauge");
}

void testAllocationDirtyAndResetSemantics() {
    minidb::test::TemporaryDatabase database("pager_stats_allocate");
    Pager pager(database.path().string());
    requireZeroCounters(pager.stats(), 0);

    const auto pageId = pager.allocatePage();
    auto stats = pager.stats();
    minidb::test::require(
        pageId == 1 && stats.appendedPages == 1 && stats.residentPages == 1
            && stats.pageRequests == 0 && stats.physicalPageWrites == 0,
        "Pager append counters or resident gauge were incorrect");

    static_cast<void>(pager.getPage(pageId));
    pager.markDirty(pageId);
    pager.flush(pageId);
    pager.flush(pageId);
    stats = pager.stats();
    minidb::test::require(
        stats.pageRequests == 1 && stats.cacheHits == 1 && stats.cacheMisses == 0
            && stats.physicalPageReads == 0 && stats.dirtyMarks == 1
            && stats.flushCalls == 2 && stats.physicalPageWrites == 1,
        "Pager hit/dirty/flush/write counters were incorrect");

    pager.resetStats();
    requireZeroCounters(pager.stats(), 1);
    minidb::test::require(pager.residentPageCount() == 1,
                          "resetStats unexpectedly cleared the page cache");
}

void testReopenMissReadThenHit() {
    minidb::test::TemporaryDatabase database("pager_stats_reopen");
    {
        Pager pager(database.path().string());
        const auto pageId = pager.allocatePage();
        auto& page = pager.getPage(pageId);
        page[0] = std::byte{0xA5};
        pager.markDirty(pageId);
        pager.flushAll();
    }
    {
        Pager pager(database.path().string());
        requireZeroCounters(pager.stats(), 0);
        minidb::test::require(pager.getPage(1)[0] == std::byte{0xA5},
                              "reopened test page payload changed");
        auto stats = pager.stats();
        minidb::test::require(
            stats.pageRequests == 1 && stats.cacheMisses == 1 && stats.cacheHits == 0
                && stats.physicalPageReads == 1 && stats.residentPages == 1,
            "first reopened lookup did not report a miss and physical read");

        static_cast<void>(pager.getPage(1));
        stats = pager.stats();
        minidb::test::require(
            stats.pageRequests == 2 && stats.cacheMisses == 1 && stats.cacheHits == 1
                && stats.physicalPageReads == 1,
            "second reopened lookup did not report a cache hit");

        minidb::test::requireThrows<std::out_of_range>(
            [&] { static_cast<void>(pager.getPage(pager.pageCount())); },
            "invalid Pager request did not fail");
        minidb::test::require(pager.stats().pageRequests == 2,
                              "rejected PageId was counted as a normal page request");
    }
}

} // namespace

int main() {
    try {
        testAllocationDirtyAndResetSemantics();
        testReopenMissReadThenHit();
        std::cout << "Pager stats tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Pager stats test failure: " << error.what() << '\n';
        return 1;
    }
}
