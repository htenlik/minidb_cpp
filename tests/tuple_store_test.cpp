#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/tuple_store.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

minidb::TupleBytes makeTuple(std::size_t size, std::uint8_t seed) {
    minidb::TupleBytes tuple(size);
    for (std::size_t index = 0; index < size; ++index) {
        tuple[index] = static_cast<std::byte>((seed + index * 29U) & 0xFFU);
    }
    return tuple;
}

std::uint32_t readUint32(const minidb::Pager::Page& page, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint64_t readUint64(const minidb::Pager::Page& page, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

void writeUint32(minidb::Pager::Page& page, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void writeUint64(minidb::Pager::Page& page, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void testEmptyHeapMetadataLayoutAndFirstInsert() {
    static_assert(minidb::tuple_heap_metadata_layout::HEADER_SIZE == 64);
    static_assert(minidb::tuple_heap_metadata_layout::FIRST_PAGE_ID_OFFSET == 16);
    static_assert(minidb::tuple_heap_metadata_layout::LAST_PAGE_ID_OFFSET == 20);
    static_assert(minidb::tuple_heap_metadata_layout::TUPLE_COUNT_OFFSET == 24);

    minidb::test::TemporaryDatabase database("tuple_store_empty_layout");
    minidb::Pager pager(database.path().string());
    auto store = minidb::TupleStore::create(pager);
    const auto metadataPageId = store.metadataPageId();
    const auto& metadata = pager.getPage(metadataPageId);
    minidb::test::require(metadataPageId == 1, "Heap metadata was not first allocatable page");
    minidb::test::require(store.empty() && store.size() == 0,
                          "New tuple heap was not empty");
    minidb::test::require(store.firstPageId() == minidb::INVALID_PAGE_ID
                              && store.lastPageId() == minidb::INVALID_PAGE_ID,
                          "Empty tuple heap allocated a data page");
    minidb::test::require(pager.pageCount() == 2,
                          "Empty tuple heap allocated more than its metadata page");
    minidb::test::require(
        std::equal(
            minidb::tuple_heap_metadata_layout::MAGIC.begin(),
            minidb::tuple_heap_metadata_layout::MAGIC.end(),
            metadata.begin()),
        "Tuple heap metadata magic was not encoded");
    minidb::test::require(
        readUint32(metadata, minidb::tuple_heap_metadata_layout::LAYOUT_VERSION_OFFSET) == 1
            && readUint32(metadata, minidb::tuple_heap_metadata_layout::HEADER_SIZE_OFFSET) == 64,
        "Tuple heap metadata version/header were not encoded little-endian");
    minidb::test::require(
        readUint32(metadata, minidb::tuple_heap_metadata_layout::FIRST_PAGE_ID_OFFSET)
                == minidb::INVALID_PAGE_ID
            && readUint32(metadata, minidb::tuple_heap_metadata_layout::LAST_PAGE_ID_OFFSET)
                == minidb::INVALID_PAGE_ID
            && readUint64(metadata, minidb::tuple_heap_metadata_layout::TUPLE_COUNT_OFFSET) == 0,
        "Empty tuple heap metadata fields were incorrect");

    const auto tuple = makeTuple(37, 1);
    const auto rid = store.insert(tuple);
    minidb::test::require(rid == minidb::RecordId{2, 0},
                          "First tuple did not receive expected PageId/SlotId");
    minidb::test::require(store.firstPageId() == rid.pageId
                              && store.lastPageId() == rid.pageId,
                          "First insert did not establish both heap endpoints");
    minidb::test::require(store.get(rid) == tuple && store.size() == 1,
                          "First tuple did not round trip through TupleStore");
    store.validate();
}

void testFirstFitUpdateDeleteScanAndSlotReuse() {
    minidb::test::TemporaryDatabase database("tuple_store_first_fit");
    minidb::Pager pager(database.path().string());
    auto store = minidb::TupleStore::create(pager);
    const auto first = makeTuple(2000, 1);
    const auto second = makeTuple(1500, 2);
    const auto third = makeTuple(1000, 3);
    const auto firstRid = store.insert(first);
    const auto secondRid = store.insert(second);
    const auto thirdRid = store.insert(third);
    minidb::test::require(firstRid.pageId == secondRid.pageId
                              && thirdRid.pageId != firstRid.pageId,
                          "TupleStore did not create the expected two-page heap");

    const auto before = store.scan();
    minidb::test::require(
        before == std::vector<minidb::TupleStore::ScanEntry>{
                      {firstRid, first}, {secondRid, second}, {thirdRid, third}},
        "TupleStore scan was not page/SlotId ordered");

    store.erase(firstRid);
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(store.get(firstRid)); },
        "Deleted tuple remained readable");
    const auto replacement = makeTuple(1800, 4);
    const auto reused = store.insert(replacement);
    minidb::test::require(reused == firstRid,
                          "First-fit insertion did not reuse the deleted slot on first page");

    const auto updated = makeTuple(1200, 5);
    minidb::test::require(store.tryUpdate(secondRid, updated),
                          "TupleStore in-page update failed");
    minidb::test::require(store.get(secondRid) == updated,
                          "TupleStore update changed RID or payload incorrectly");
    minidb::test::require(store.size() == 3, "TupleStore mutation changed tuple count incorrectly");
    store.validate();
}

void testNoCrossPageUpdateRelocation() {
    minidb::test::TemporaryDatabase database("tuple_store_no_relocation");
    minidb::Pager pager(database.path().string());
    auto store = minidb::TupleStore::create(pager);
    const auto original = makeTuple(100, 1);
    const auto rid = store.insert(original);
    static_cast<void>(store.insert(makeTuple(3800, 2)));
    const auto pageCount = pager.pageCount();
    minidb::test::require(!store.tryUpdate(rid, makeTuple(300, 3)),
                          "TupleStore relocated an update that could not fit in-page");
    minidb::test::require(store.get(rid) == original,
                          "Failed TupleStore update changed the original tuple");
    minidb::test::require(pager.pageCount() == pageCount,
                          "Failed TupleStore update allocated another page");
    store.validate();
}

void testEmptyPageReclamationAndEndpointRepair() {
    minidb::test::TemporaryDatabase database("tuple_store_reclaim");
    minidb::Pager pager(database.path().string());
    auto store = minidb::TupleStore::create(pager);
    const auto tuple = makeTuple(minidb::slotted_page_layout::MAX_TUPLE_SIZE, 1);
    const auto first = store.insert(tuple);
    const auto middle = store.insert(tuple);
    const auto last = store.insert(tuple);
    minidb::test::require(store.reachablePageIds().size() == 3,
                          "Reclamation fixture did not create three pages");

    store.erase(middle);
    minidb::PageAllocator allocator(pager);
    const auto freeAfterMiddle = allocator.freePageIds();
    minidb::test::require(
        std::find(freeAfterMiddle.begin(), freeAfterMiddle.end(), middle.pageId)
            != freeAfterMiddle.end(),
        "Empty middle tuple page was not reclaimed");
    minidb::SlottedPage firstPage(pager, first.pageId);
    minidb::SlottedPage lastPage(pager, last.pageId);
    minidb::test::require(firstPage.nextPageId() == last.pageId
                              && lastPage.previousPageId() == first.pageId,
                          "Middle-page reclamation did not repair both links");

    store.erase(first);
    minidb::test::require(store.firstPageId() == last.pageId,
                          "First-page reclamation did not update heap metadata");
    store.erase(last);
    minidb::test::require(store.empty()
                              && store.firstPageId() == minidb::INVALID_PAGE_ID
                              && store.lastPageId() == minidb::INVALID_PAGE_ID,
                          "Final deletion did not restore empty heap representation");
    minidb::test::require(store.reachablePageIds().empty(),
                          "Empty heap retained reachable data pages");
    store.validate();

    const auto replacement = makeTuple(73, 9);
    const auto replacementRid = store.insert(replacement);
    minidb::test::require(store.get(replacementRid) == replacement,
                          "Reinsert after empty failed");
    store.validate();
}

void testReopenPersistenceAndRepeatedMutation() {
    minidb::test::TemporaryDatabase database("tuple_store_reopen");
    minidb::PageId metadataPageId = minidb::INVALID_PAGE_ID;
    minidb::RecordId first{};
    minidb::RecordId second{};
    auto firstTuple = makeTuple(53, 1);
    auto secondTuple = makeTuple(1700, 2);
    {
        minidb::Pager pager(database.path().string());
        auto store = minidb::TupleStore::create(pager);
        metadataPageId = store.metadataPageId();
        first = store.insert(firstTuple);
        second = store.insert(secondTuple);
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        minidb::PageAllocator allocator(pager);
        allocator.validate();
        auto store = minidb::TupleStore::open(pager, metadataPageId);
        minidb::test::require(store.get(first) == firstTuple && store.get(second) == secondTuple,
                              "Tuple bytes or RIDs did not persist across reopen");
        firstTuple = makeTuple(400, 3);
        minidb::test::require(store.tryUpdate(first, firstTuple),
                              "Update after reopen failed");
        store.erase(second);
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto store = minidb::TupleStore::open(pager, metadataPageId);
        minidb::test::require(store.size() == 1 && store.get(first) == firstTuple,
                              "Reopened update/delete state was incorrect");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(store.get(second)); },
            "Reopened heap returned a deleted tuple");
        store.validate();
    }
}

void testHeapOwnershipAndInvalidRids() {
    minidb::test::TemporaryDatabase database("tuple_store_ownership");
    minidb::Pager pager(database.path().string());
    auto first = minidb::TupleStore::create(pager);
    auto second = minidb::TupleStore::create(pager);
    const auto firstRid = first.insert(makeTuple(10, 1));
    const auto secondRid = second.insert(makeTuple(10, 2));
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(first.get(secondRid)); },
        "TupleStore accepted a RID owned by another heap");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(first.get(minidb::INVALID_RECORD_ID)); },
        "TupleStore accepted INVALID_RECORD_ID");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(first.get(minidb::RecordId{firstRid.pageId, 99})); },
        "TupleStore accepted a SlotId outside the page directory");
    minidb::test::require(first.get(firstRid) == makeTuple(10, 1),
                          "Invalid-RID checks mutated the owning heap");
}

void testCrossComponentPageReuse() {
    minidb::test::TemporaryDatabase database("tuple_store_cross_reuse");
    minidb::Pager pager(database.path().string());
    auto heap = minidb::TupleStore::create(pager);
    minidb::PageAllocator allocator(pager);
    const auto recordPageId = allocator.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
    const minidb::RecordId indexedRid{recordPageId, 1};
    static_cast<void>(tree.insert(7, indexedRid));
    const auto formerLeafPageId = tree.rootPageId();
    minidb::test::require(tree.erase(7), "Cross-reuse B+ tree erase failed");

    const auto tupleRid = heap.insert(makeTuple(200, 4));
    minidb::test::require(tupleRid.pageId == formerLeafPageId,
                          "Freed B+ tree leaf was not reused as a SlottedPage");
    heap.erase(tupleRid);
    static_cast<void>(tree.insert(8, indexedRid));
    minidb::test::require(tree.rootPageId() == formerLeafPageId,
                          "Freed SlottedPage was not reused as a B+ tree leaf");
    heap.validate();
    tree.validate();
}

template <typename Mutator>
void requireMetadataCorruption(std::string_view name, Mutator&& mutate, bool insert = false) {
    minidb::test::TemporaryDatabase database(name);
    minidb::Pager pager(database.path().string());
    auto store = minidb::TupleStore::create(pager);
    if (insert) {
        static_cast<void>(store.insert(makeTuple(20, 1)));
    }
    auto& metadata = pager.getPage(store.metadataPageId());
    mutate(pager, store, metadata);
    pager.markDirty(store.metadataPageId());
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(minidb::TupleStore::open(pager, store.metadataPageId())); },
        "TupleStore accepted corrupt heap metadata");
}

void testMetadataAndChainCorruption() {
    requireMetadataCorruption("tuple_heap_bad_magic", [](auto&, auto&, auto& metadata) {
        metadata[minidb::tuple_heap_metadata_layout::MAGIC_OFFSET] = std::byte{'X'};
    });
    requireMetadataCorruption("tuple_heap_bad_version", [](auto&, auto&, auto& metadata) {
        writeUint32(
            metadata,
            minidb::tuple_heap_metadata_layout::LAYOUT_VERSION_OFFSET,
            minidb::tuple_heap_metadata_layout::CURRENT_VERSION + 1);
    });
    requireMetadataCorruption("tuple_heap_dangling_first", [](auto& pager, auto&, auto& metadata) {
        writeUint32(
            metadata,
            minidb::tuple_heap_metadata_layout::FIRST_PAGE_ID_OFFSET,
            pager.pageCount() + 5);
        writeUint32(
            metadata,
            minidb::tuple_heap_metadata_layout::LAST_PAGE_ID_OFFSET,
            pager.pageCount() + 5);
        writeUint64(metadata, minidb::tuple_heap_metadata_layout::TUPLE_COUNT_OFFSET, 1);
    });
    requireMetadataCorruption("tuple_heap_count_mismatch", [](auto&, auto& store, auto& metadata) {
        writeUint64(
            metadata,
            minidb::tuple_heap_metadata_layout::TUPLE_COUNT_OFFSET,
            store.size() + 1);
    }, true);

    for (const bool corruptNext : {true, false}) {
        minidb::test::TemporaryDatabase database(
            corruptNext ? "tuple_heap_dangling_next" : "tuple_heap_dangling_previous");
        minidb::Pager pager(database.path().string());
        auto store = minidb::TupleStore::create(pager);
        const auto rid = store.insert(makeTuple(20, 1));
        auto& page = pager.getPage(rid.pageId);
        writeUint32(
            page,
            corruptNext ? minidb::slotted_page_layout::NEXT_PAGE_ID_OFFSET
                        : minidb::slotted_page_layout::PREVIOUS_PAGE_ID_OFFSET,
            pager.pageCount() + 10);
        pager.markDirty(rid.pageId);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { store.validate(); },
            "TupleStore accepted a dangling heap-page link");
    }

    {
        minidb::test::TemporaryDatabase database("tuple_heap_inconsistent_links");
        minidb::Pager pager(database.path().string());
        auto store = minidb::TupleStore::create(pager);
        const auto tuple = makeTuple(minidb::slotted_page_layout::MAX_TUPLE_SIZE, 1);
        const auto first = store.insert(tuple);
        const auto second = store.insert(tuple);
        minidb::SlottedPage secondPage(pager, second.pageId);
        secondPage.setPreviousPageId(minidb::INVALID_PAGE_ID);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { store.validate(); },
            "TupleStore accepted inconsistent backward links");
        static_cast<void>(first);
    }
    {
        minidb::test::TemporaryDatabase database("tuple_heap_cycle");
        minidb::Pager pager(database.path().string());
        auto store = minidb::TupleStore::create(pager);
        const auto tuple = makeTuple(minidb::slotted_page_layout::MAX_TUPLE_SIZE, 1);
        const auto first = store.insert(tuple);
        static_cast<void>(store.insert(tuple));
        const auto third = store.insert(tuple);
        minidb::SlottedPage firstPage(pager, first.pageId);
        minidb::SlottedPage thirdPage(pager, third.pageId);
        thirdPage.setNextPageId(first.pageId);
        firstPage.setPreviousPageId(third.pageId);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { store.validate(); },
            "TupleStore accepted a cyclic page chain");
    }
}

} // namespace

int main() {
    try {
        testEmptyHeapMetadataLayoutAndFirstInsert();
        testFirstFitUpdateDeleteScanAndSlotReuse();
        testNoCrossPageUpdateRelocation();
        testEmptyPageReclamationAndEndpointRepair();
        testReopenPersistenceAndRepeatedMutation();
        testHeapOwnershipAndInvalidRids();
        testCrossComponentPageReuse();
        testMetadataAndChainCorruption();
        std::cout << "tuple_store_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tuple_store_test failed: " << error.what() << '\n';
        return 1;
    }
}
