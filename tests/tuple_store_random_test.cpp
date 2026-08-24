#include "minidb/page_allocator.hpp"
#include "minidb/tuple_store.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct RecordIdLess {
    bool operator()(minidb::RecordId left, minidb::RecordId right) const noexcept {
        return left.pageId < right.pageId
            || (left.pageId == right.pageId && left.slotId < right.slotId);
    }
};

using TupleModel = std::map<minidb::RecordId, minidb::TupleBytes, RecordIdLess>;

struct PageModel {
    std::uint16_t slotCount = 0;
};

minidb::TupleBytes makeTuple(std::size_t size, std::mt19937& random) {
    minidb::TupleBytes tuple(size);
    for (auto& byte : tuple) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    return tuple;
}

std::size_t randomTupleSize(std::mt19937& random) {
    const auto category = random() % 100U;
    if (category < 35U) {
        return 1 + random() % 16U;
    }
    if (category < 70U) {
        return 17 + random() % 112U;
    }
    if (category < 88U) {
        return 129 + random() % 384U;
    }
    if (category < 97U) {
        return 513 + random() % 988U;
    }
    return 3000 + random() % (minidb::slotted_page_layout::MAX_TUPLE_SIZE - 2999U);
}

TupleModel::iterator selectLive(TupleModel& model, std::mt19937& random) {
    auto position = model.begin();
    std::advance(position, static_cast<std::ptrdiff_t>(random() % model.size()));
    return position;
}

std::size_t pagePayloadSize(const TupleModel& tuples, minidb::PageId pageId) {
    std::size_t total = 0;
    for (const auto& [rid, tuple] : tuples) {
        if (rid.pageId == pageId) {
            total += tuple.size();
        }
    }
    return total;
}

bool pageHasFreeSlot(
    const TupleModel& tuples,
    minidb::PageId pageId,
    std::uint16_t slotCount) {
    for (std::size_t slot = 0; slot < slotCount; ++slot) {
        if (!tuples.contains(minidb::RecordId{pageId, static_cast<minidb::SlotId>(slot)})) {
            return true;
        }
    }
    return false;
}

std::size_t modeledFreeSpace(
    const TupleModel& tuples,
    minidb::PageId pageId,
    const PageModel& page) {
    return minidb::database_format::PAGE_SIZE
        - minidb::slotted_page_layout::HEADER_SIZE
        - (static_cast<std::size_t>(page.slotCount) * minidb::slotted_page_layout::SLOT_SIZE)
        - pagePayloadSize(tuples, pageId);
}

std::vector<minidb::TupleStore::ScanEntry> expectedScan(
    const TupleModel& tuples,
    const std::vector<minidb::PageId>& pageOrder,
    const std::unordered_map<minidb::PageId, PageModel>& pages) {
    std::vector<minidb::TupleStore::ScanEntry> result;
    result.reserve(tuples.size());
    for (const auto pageId : pageOrder) {
        const auto page = pages.at(pageId);
        for (std::size_t slot = 0; slot < page.slotCount; ++slot) {
            const minidb::RecordId rid{pageId, static_cast<minidb::SlotId>(slot)};
            if (const auto position = tuples.find(rid); position != tuples.end()) {
                result.emplace_back(rid, position->second);
            }
        }
    }
    return result;
}

void validateModel(
    const minidb::TupleStore& store,
    const TupleModel& tuples,
    const std::vector<minidb::PageId>& pageOrder,
    const std::unordered_map<minidb::PageId, PageModel>& pages,
    minidb::PageAllocator& allocator,
    minidb::BufferPoolManager& bufferPool) {
    store.validate();
    allocator.validate();
    minidb::test::require(store.size() == tuples.size(), "Random tuple-store size differs");
    const auto expected = expectedScan(tuples, pageOrder, pages);
    const auto firstScan = store.scan();
    minidb::test::require(firstScan == expected, "Random tuple-store scan differs from model");
    minidb::test::require(store.scan() == firstScan, "Random tuple-store scan order is unstable");
    for (const auto& [rid, tuple] : tuples) {
        minidb::test::require(store.get(rid) == tuple, "Random tuple-store lookup differs");
    }
    const auto heapPages = store.reachablePageIds();
    const auto freePages = allocator.freePageIds();
    minidb::test::require(heapPages == pageOrder, "Modeled heap page order differs");
    for (const auto pageId : heapPages) {
        minidb::test::require(
            std::find(freePages.begin(), freePages.end(), pageId) == freePages.end(),
            "Random workload found a page both live and free");
    }
    bufferPool.validate();
    bufferPool.validateReplacer();
    minidb::test::require(
        bufferPool.stats().pinnedFrames == 0,
        "Random TupleStore operation leaked a page pin");
}

std::string context(
    std::uint32_t seed,
    std::size_t operation,
    std::string_view operationName,
    minidb::RecordId rid,
    std::size_t tupleSize,
    std::size_t reopenCount) {
    std::ostringstream stream;
    stream << "seed=" << seed << " operation=" << operation
           << " type=" << operationName << " rid=" << rid.pageId << ':' << rid.slotId
           << " tuple_length=" << tupleSize << " reopen_count=" << reopenCount;
    return stream.str();
}

void runRandomWorkload(std::uint32_t seed) {
    constexpr std::size_t OPERATION_COUNT = 10'000;
    constexpr std::size_t REOPEN_INTERVAL = 125;
    minidb::test::TemporaryDatabase database("tuple_store_random");
    std::mt19937 random(seed);
    TupleModel tuples;
    std::vector<minidb::PageId> pageOrder;
    std::unordered_map<minidb::PageId, PageModel> pages;
    minidb::PageId metadataPageId = minidb::INVALID_PAGE_ID;
    std::size_t reopenCount = 0;

    for (std::size_t batch = 0; batch < OPERATION_COUNT; batch += REOPEN_INTERVAL) {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto& allocator = storage.allocator;
        auto store = batch == 0
            ? minidb::TupleStore::create(storage.bufferPool, storage.diskManager, storage.allocator)
            : minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, metadataPageId);
        if (batch == 0) {
            metadataPageId = store.metadataPageId();
        } else {
            ++reopenCount;
        }
        validateModel(store, tuples, pageOrder, pages, allocator, storage.bufferPool);

        const auto end = std::min(batch + REOPEN_INTERVAL, OPERATION_COUNT);
        for (std::size_t operation = batch; operation < end; ++operation) {
            const auto choice = random() % 100U;
            std::string_view operationName = "scan";
            minidb::RecordId selectedRid = minidb::INVALID_RECORD_ID;
            std::size_t selectedSize = 0;
            try {
                if (choice < 38U || tuples.empty()) {
                    operationName = "insert";
                    selectedSize = randomTupleSize(random);
                    auto tuple = makeTuple(selectedSize, random);

                    std::optional<minidb::PageId> expectedPage;
                    minidb::SlotId expectedSlot = 0;
                    for (const auto pageId : pageOrder) {
                        const auto& page = pages.at(pageId);
                        const bool reusable = pageHasFreeSlot(tuples, pageId, page.slotCount);
                        const auto required = selectedSize
                            + (reusable ? 0 : minidb::slotted_page_layout::SLOT_SIZE);
                        if (required <= modeledFreeSpace(tuples, pageId, page)) {
                            expectedPage = pageId;
                            if (reusable) {
                                for (std::size_t slot = 0; slot < page.slotCount; ++slot) {
                                    const minidb::RecordId candidate{
                                        pageId, static_cast<minidb::SlotId>(slot)};
                                    if (!tuples.contains(candidate)) {
                                        expectedSlot = candidate.slotId;
                                        break;
                                    }
                                }
                            } else {
                                expectedSlot = page.slotCount;
                            }
                            break;
                        }
                    }

                    selectedRid = store.insert(tuple);
                    if (expectedPage) {
                        minidb::test::require(
                            selectedRid.pageId == *expectedPage && selectedRid.slotId == expectedSlot,
                            "Random first-fit insertion chose an unexpected page/slot");
                    } else {
                        minidb::test::require(selectedRid.slotId == 0,
                                              "New random heap page did not start at slot zero");
                        pageOrder.push_back(selectedRid.pageId);
                        pages.emplace(selectedRid.pageId, PageModel{});
                    }
                    auto& page = pages.at(selectedRid.pageId);
                    if (selectedRid.slotId == page.slotCount) {
                        ++page.slotCount;
                    }
                    tuples[selectedRid] = std::move(tuple);
                } else if (choice < 56U) {
                    operationName = "get";
                    const auto position = selectLive(tuples, random);
                    selectedRid = position->first;
                    selectedSize = position->second.size();
                    minidb::test::require(store.get(selectedRid) == position->second,
                                          "Random get returned wrong tuple");
                } else if (choice < 79U) {
                    operationName = "update";
                    auto position = selectLive(tuples, random);
                    selectedRid = position->first;
                    selectedSize = randomTupleSize(random);
                    auto replacement = makeTuple(selectedSize, random);
                    const auto& page = pages.at(selectedRid.pageId);
                    const bool expectedResult = selectedSize
                        <= modeledFreeSpace(tuples, selectedRid.pageId, page)
                            + position->second.size();
                    const auto original = position->second;
                    const bool result = store.tryUpdate(selectedRid, replacement);
                    minidb::test::require(result == expectedResult,
                                          "Random update capacity result differs from model");
                    if (result) {
                        position->second = std::move(replacement);
                    } else {
                        minidb::test::require(store.get(selectedRid) == original,
                                              "Failed random update changed original tuple");
                    }
                } else if (choice < 96U) {
                    operationName = "erase";
                    const auto position = selectLive(tuples, random);
                    selectedRid = position->first;
                    selectedSize = position->second.size();
                    store.erase(selectedRid);
                    tuples.erase(position);
                    const bool pageStillLive = std::any_of(
                        tuples.begin(), tuples.end(), [&](const auto& entry) {
                            return entry.first.pageId == selectedRid.pageId;
                        });
                    if (!pageStillLive) {
                        pageOrder.erase(
                            std::find(pageOrder.begin(), pageOrder.end(), selectedRid.pageId));
                        pages.erase(selectedRid.pageId);
                    }
                } else {
                    operationName = "scan";
                    minidb::test::require(
                        store.scan() == expectedScan(tuples, pageOrder, pages),
                        "Random explicit scan differs from model");
                }

                if (operation % 50 == 0) {
                    validateModel(store, tuples, pageOrder, pages, allocator, storage.bufferPool);
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    context(
                        seed,
                        operation,
                        operationName,
                        selectedRid,
                        selectedSize,
                        reopenCount)
                    + ": " + error.what());
            }
        }
        validateModel(store, tuples, pageOrder, pages, allocator, storage.bufferPool);
        storage.bufferPool.flushAll();
    }
}

void testRandomizedModelAcrossReopens() {
    constexpr std::array<std::uint32_t, 2> SEEDS{{0x5A000001U, 0x5A00C0DEU}};
    for (const auto seed : SEEDS) {
        runRandomWorkload(seed);
    }
}

} // namespace

int main() {
    try {
        testRandomizedModelAcrossReopens();
        std::cout << "tuple_store_random_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tuple_store_random_test failed: " << error.what() << '\n';
        return 1;
    }
}
