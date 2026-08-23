#include "minidb/bplus_tree.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ReferenceIndex = std::map<minidb::IndexKey, minidb::RecordId>;

minidb::RecordId makeRid(
    minidb::IndexKey key,
    minidb::PageId recordPageId,
    std::uint32_t salt = 0) {
    return minidb::RecordId{
        recordPageId,
        static_cast<minidb::SlotId>((key + salt) % 60'000U),
    };
}

std::uint32_t readUint32(const minidb::Pager::Page& page, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

void writeUint32(
    minidb::Pager::Page& page,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

std::vector<minidb::IndexEntry> expectedEntries(const ReferenceIndex& reference) {
    std::vector<minidb::IndexEntry> result;
    result.reserve(reference.size());
    for (const auto& [key, rid] : reference) {
        result.push_back(minidb::IndexEntry{key, rid});
    }
    return result;
}

std::vector<minidb::IndexEntry> expectedRange(
    const ReferenceIndex& reference,
    minidb::IndexKey lower,
    minidb::IndexKey upper) {
    std::vector<minidb::IndexEntry> result;
    if (lower > upper) {
        return result;
    }
    for (auto position = reference.lower_bound(lower);
         position != reference.end() && position->first <= upper;
         ++position) {
        result.push_back(minidb::IndexEntry{position->first, position->second});
    }
    return result;
}

void requireMatches(
    const minidb::PersistentBPlusTree& tree,
    const ReferenceIndex& reference) {
    tree.validate();
    minidb::test::require(tree.size() == reference.size(), "Persistent tree size differs");
    minidb::test::require(tree.empty() == reference.empty(), "Persistent empty state differs");
    minidb::test::require(
        tree.scanAll() == expectedEntries(reference),
        "Persistent full scan differs from reference");
}

void insertRange(
    minidb::PersistentBPlusTree& tree,
    ReferenceIndex& reference,
    minidb::PageId recordPageId,
    minidb::IndexKey count,
    std::uint32_t salt = 0) {
    for (minidb::IndexKey key = 0; key < count; ++key) {
        const auto rid = makeRid(key, recordPageId, salt);
        minidb::test::require(tree.insert(key, rid), "Persistent range insertion failed");
        reference.emplace(key, rid);
    }
}

void testMissingSimpleAndFinalErase() {
    minidb::test::TemporaryDatabase database("persistent_delete_simple");
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);

    minidb::test::require(!tree.erase(7), "Empty persistent tree erased a missing key");
    const auto rid = makeRid(7, recordPageId);
    minidb::test::require(tree.insert(7, rid), "Single-key insert failed");
    const auto leafRoot = tree.rootPageId();
    minidb::test::require(!tree.erase(8), "Persistent tree erased a missing key");
    minidb::test::require(tree.size() == 1 && tree.find(7) == rid,
                          "Missing erase mutated the tree");
    minidb::test::require(tree.erase(7), "Final-key erase failed");
    minidb::test::require(tree.empty(), "Final-key erase did not empty the tree");
    minidb::test::require(tree.rootPageId() == minidb::INVALID_PAGE_ID,
                          "Final-key erase retained a root page");
    minidb::test::require(tree.height() == 0, "Empty persistent tree retained height");
    minidb::PageAllocator allocator(pager);
    const auto freePages = allocator.freePageIds();
    minidb::test::require(
        std::find(freePages.begin(), freePages.end(), leafRoot) != freePages.end(),
        "Final leaf root was not reclaimed");
    tree.validate();
}

void testTargetedLeafBorrowMergeAndSeparatorRepair() {
    const auto runCase = [](
                             std::string_view name,
                             const std::vector<minidb::IndexKey>& inserted,
                             minidb::IndexKey erased,
                             bool expectReclamation) {
        minidb::test::TemporaryDatabase database(name);
        minidb::Pager pager(database.path().string());
        const auto recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        ReferenceIndex reference;
        for (const auto key : inserted) {
            const auto rid = makeRid(key, recordPageId, 7);
            static_cast<void>(tree.insert(key, rid));
            reference.emplace(key, rid);
        }
        const auto freeBefore = minidb::PageAllocator(pager).freePageIds().size();
        minidb::test::require(tree.erase(erased), "Targeted leaf erase failed");
        reference.erase(erased);
        requireMatches(tree, reference);
        const auto freeAfter = minidb::PageAllocator(pager).freePageIds().size();
        minidb::test::require(
            (freeAfter > freeBefore) == expectReclamation,
            "Targeted leaf case did not perform the expected borrow/merge behavior");
        for (const auto& [key, rid] : reference) {
            minidb::test::require(tree.find(key) == rid, "Surviving RID association changed");
        }
    };

    runCase("persistent_leaf_borrow_right", {1, 2, 3, 4, 5}, 1, false);
    runCase("persistent_leaf_borrow_left", {1, 2, 3, 4, 0}, 4, false);
    runCase("persistent_leaf_merge_right", {1, 2, 3, 4}, 1, true);
    runCase("persistent_leaf_merge_left", {1, 2, 3, 4}, 4, true);

    minidb::test::TemporaryDatabase database("persistent_separator_min_change");
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 4, 4);
    ReferenceIndex reference;
    for (minidb::IndexKey key = 0; key < 6; ++key) {
        const auto rid = makeRid(key, recordPageId, 19);
        static_cast<void>(tree.insert(key, rid));
        reference.emplace(key, rid);
    }
    minidb::test::require(tree.erase(3), "Right-child minimum erase failed");
    reference.erase(3);
    requireMatches(tree, reference);
    minidb::test::require(tree.erase(5), "Maximum-key erase failed");
    reference.erase(5);
    requireMatches(tree, reference);
}

void runEraseOrder(std::vector<minidb::IndexKey> order, std::string_view name) {
    minidb::test::TemporaryDatabase database(name);
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
    ReferenceIndex reference;
    insertRange(tree, reference, recordPageId, static_cast<minidb::IndexKey>(order.size()), 13);
    minidb::test::require(tree.height() >= 4, "Erase fixture did not create a deep tree");

    auto previousHeight = tree.height();
    bool rootShrank = false;
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto key = order[index];
        minidb::test::require(tree.erase(key), "Existing key erase failed");
        reference.erase(key);
        const auto height = tree.height();
        rootShrank = rootShrank || height < previousHeight;
        previousHeight = height;
        if (index % 3 == 0 || reference.empty()) {
            requireMatches(tree, reference);
        } else {
            tree.validate();
        }
    }
    minidb::test::require(rootShrank, "Adversarial erase order never shrank the root");
    minidb::test::require(tree.rootPageId() == minidb::INVALID_PAGE_ID,
                          "Delete-all did not restore the empty representation");

    const auto replacement = makeRid(999, recordPageId, 3);
    minidb::test::require(tree.insert(999, replacement), "Reinsert after delete-all failed");
    minidb::test::require(tree.find(999) == replacement, "Reinserted RID was not found");
    tree.validate();
}

void testAdversarialEraseOrdersAndRebalancing() {
    constexpr minidb::IndexKey COUNT = 180;
    std::vector<minidb::IndexKey> ascending(COUNT);
    for (minidb::IndexKey key = 0; key < COUNT; ++key) {
        ascending[key] = key;
    }
    runEraseOrder(ascending, "persistent_delete_ascending");

    auto descending = ascending;
    std::reverse(descending.begin(), descending.end());
    runEraseOrder(descending, "persistent_delete_descending");

    std::vector<minidb::IndexKey> alternating;
    alternating.reserve(COUNT);
    for (minidb::IndexKey key = 0; key < COUNT; key += 2) {
        alternating.push_back(key);
    }
    for (minidb::IndexKey key = 1; key < COUNT; key += 2) {
        alternating.push_back(key);
    }
    runEraseOrder(alternating, "persistent_delete_alternating");
}

void testReopenDeletionAndPageReuse() {
    minidb::test::TemporaryDatabase database("persistent_delete_reopen_reuse");
    minidb::PageId metadataPageId = minidb::INVALID_PAGE_ID;
    minidb::PageId recordPageId = minidb::INVALID_PAGE_ID;
    minidb::PageId highWaterPageCount = 0;
    std::vector<minidb::PageId> originalTreePages;
    std::vector<minidb::PageId> reclaimedTreePages;
    ReferenceIndex reference;

    {
        minidb::Pager pager(database.path().string());
        recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        metadataPageId = tree.metadataPageId();
        insertRange(tree, reference, recordPageId, 260, 29);
        highWaterPageCount = pager.pageCount();
        originalTreePages = tree.reachableNodePageIds();
        requireMatches(tree, reference);
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::open(pager, metadataPageId);
        for (minidb::IndexKey key = 0; key < 260; ++key) {
            minidb::test::require(tree.erase(key), "Erase after reopen failed");
            reference.erase(key);
        }
        requireMatches(tree, reference);
        minidb::PageAllocator allocator(pager);
        const auto freePages = allocator.freePageIds();
        minidb::test::require(!freePages.empty(),
                              "Delete-all did not populate the free list");
        for (const auto pageId : originalTreePages) {
            if (std::find(freePages.begin(), freePages.end(), pageId) != freePages.end()) {
                reclaimedTreePages.push_back(pageId);
            }
        }
        minidb::test::require(
            reclaimedTreePages.size() == originalTreePages.size(),
            "Delete-all did not reclaim every former tree node page");
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::open(pager, metadataPageId);
        insertRange(tree, reference, recordPageId, 260, 41);
        requireMatches(tree, reference);
        minidb::test::require(
            pager.pageCount() == highWaterPageCount,
            "Equivalent refill grew the file instead of reusing reclaimed index pages");
        const auto refillPages = tree.reachableNodePageIds();
        minidb::test::require(
            std::any_of(
                refillPages.begin(),
                refillPages.end(),
                [&](minidb::PageId pageId) {
                    return std::find(
                               reclaimedTreePages.begin(),
                               reclaimedTreePages.end(),
                               pageId)
                        != reclaimedTreePages.end();
                }),
            "Refill did not reuse any explicitly reclaimed tree PageId");
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::open(pager, metadataPageId);
        requireMatches(tree, reference);
    }
}

void testIndependentIndexesShareAllocatorSafely() {
    minidb::test::TemporaryDatabase database("persistent_delete_two_indexes");
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto first = minidb::PersistentBPlusTree::create(pager, 3, 3);
    auto second = minidb::PersistentBPlusTree::create(pager, 4, 4);
    ReferenceIndex firstReference;
    ReferenceIndex secondReference;
    insertRange(first, firstReference, recordPageId, 100, 1);
    for (minidb::IndexKey key = 1'000; key < 1'100; ++key) {
        const auto rid = makeRid(key, recordPageId, 2);
        minidb::test::require(second.insert(key, rid), "Second index insertion failed");
        secondReference.emplace(key, rid);
    }
    const auto secondPages = second.reachableNodePageIds();

    for (minidb::IndexKey key = 0; key < 100; ++key) {
        minidb::test::require(first.erase(key), "First index deletion failed");
    }
    firstReference.clear();
    requireMatches(first, firstReference);
    requireMatches(second, secondReference);
    const auto freePages = minidb::PageAllocator(pager).freePageIds();
    for (const auto pageId : secondPages) {
        minidb::test::require(
            std::find(freePages.begin(), freePages.end(), pageId) == freePages.end(),
            "Deleting one index reclaimed a page owned by another index");
    }
    minidb::test::require(
        std::find(freePages.begin(), freePages.end(), second.metadataPageId())
            == freePages.end(),
        "An active index metadata page was reclaimed");
}

void testDeletionEraCorruptionIsRejected() {
    {
        minidb::test::TemporaryDatabase database("persistent_underfull_leaf");
        minidb::Pager pager(database.path().string());
        const auto recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 4, 4);
        for (minidb::IndexKey key = 0; key < 8; ++key) {
            static_cast<void>(tree.insert(key, makeRid(key, recordPageId)));
        }
        const auto& root = pager.getPage(tree.rootPageId());
        const auto leafPageId = readUint32(
            root, minidb::persistent_bplus_internal_layout::childOffset(0));
        auto& leaf = pager.getPage(leafPageId);
        writeUint32(leaf, minidb::persistent_bplus_leaf_layout::KEY_COUNT_OFFSET, 1);
        std::fill(
            leaf.begin() + minidb::persistent_bplus_leaf_layout::entryOffset(1),
            leaf.end(),
            std::byte{0});
        pager.markDirty(leafPageId);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Validator accepted an underfull non-root leaf");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_underfull_internal");
        minidb::Pager pager(database.path().string());
        const auto recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 4, 4);
        for (minidb::IndexKey key = 0; key < 120; ++key) {
            static_cast<void>(tree.insert(key, makeRid(key, recordPageId)));
        }
        minidb::test::require(tree.height() >= 3, "Internal corruption fixture was too shallow");
        const auto& root = pager.getPage(tree.rootPageId());
        const auto internalPageId = readUint32(
            root, minidb::persistent_bplus_internal_layout::childOffset(0));
        auto& internal = pager.getPage(internalPageId);
        writeUint32(internal, minidb::persistent_bplus_internal_layout::KEY_COUNT_OFFSET, 1);
        writeUint32(internal, minidb::persistent_bplus_internal_layout::CHILD_COUNT_OFFSET, 2);
        const auto usedSize = minidb::persistent_bplus_internal_layout::HEADER_SIZE
            + minidb::persistent_bplus_internal_layout::PAGE_ID_SIZE
            + minidb::persistent_bplus_internal_layout::KEY_CHILD_PAIR_SIZE;
        std::fill(internal.begin() + usedSize, internal.end(), std::byte{0});
        pager.markDirty(internalPageId);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Validator accepted an underfull non-root internal page");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_live_free_overlap");
        minidb::Pager pager(database.path().string());
        const auto recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        static_cast<void>(tree.insert(7, makeRid(7, recordPageId)));
        const auto leafPageId = tree.rootPageId();
        auto& page = pager.getPage(leafPageId);
        page.fill(std::byte{0});
        std::copy(
            minidb::free_page_layout::MAGIC.begin(),
            minidb::free_page_layout::MAGIC.end(),
            page.begin() + minidb::free_page_layout::MAGIC_OFFSET);
        writeUint32(
            page,
            minidb::free_page_layout::LAYOUT_VERSION_OFFSET,
            minidb::free_page_layout::CURRENT_VERSION);
        writeUint32(
            page,
            minidb::free_page_layout::HEADER_SIZE_OFFSET,
            minidb::free_page_layout::HEADER_SIZE);
        writeUint32(
            page,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            minidb::INVALID_PAGE_ID);
        pager.markDirty(leafPageId);
        pager.updateFreeListRootPageId(leafPageId);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Validator accepted a page reachable from both the tree and free list");
    }
}

void testRepeatedSplitMergeReuseCycles() {
    minidb::test::TemporaryDatabase database("persistent_repeated_reuse_cycles");
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 3, 4);
    minidb::PageId stablePageCount = 0;
    constexpr minidb::IndexKey KEY_COUNT = 180;

    for (std::uint32_t round = 0; round < 4; ++round) {
        for (minidb::IndexKey key = 0; key < KEY_COUNT; ++key) {
            minidb::test::require(
                tree.insert(key, makeRid(key, recordPageId, round * 101U)),
                "Repeated-cycle insert failed");
        }
        tree.validate();
        if (round == 0) {
            stablePageCount = pager.pageCount();
        } else {
            minidb::test::require(
                pager.pageCount() == stablePageCount,
                "Repeated split/merge cycle appended pages instead of reusing free pages");
        }

        for (minidb::IndexKey key = 0; key < KEY_COUNT; key += 2) {
            minidb::test::require(tree.erase(key), "Repeated-cycle even erase failed");
        }
        tree.validate();
        for (minidb::IndexKey key = 1; key < KEY_COUNT; key += 2) {
            minidb::test::require(tree.erase(key), "Repeated-cycle odd erase failed");
        }
        minidb::test::require(tree.empty(), "Repeated split/merge cycle did not empty tree");
        tree.validate();
    }
}

std::string randomContext(
    std::uint32_t seed,
    std::size_t operation,
    std::uint32_t leafCapacity,
    std::uint32_t internalCapacity,
    minidb::IndexKey key,
    bool afterReopen) {
    std::ostringstream stream;
    stream << "seed=" << seed << " operation=" << operation
           << " capacities=" << leafCapacity << '/' << internalCapacity
           << " key=" << key << " after_reopen=" << std::boolalpha << afterReopen;
    return stream.str();
}

void runDifferential(
    std::uint32_t leafCapacity,
    std::uint32_t internalCapacity,
    std::uint32_t seed) {
    constexpr std::size_t OPERATION_COUNT = 2'500;
    constexpr std::size_t REOPEN_INTERVAL = 125;
    constexpr minidb::IndexKey KEY_DOMAIN = 1'024;
    minidb::test::TemporaryDatabase database("persistent_delete_differential");
    ReferenceIndex reference;
    minidb::BPlusTree memory(leafCapacity, internalCapacity);
    std::mt19937 random(seed);
    minidb::PageId metadataPageId = minidb::INVALID_PAGE_ID;
    minidb::PageId recordPageId = minidb::INVALID_PAGE_ID;

    for (std::size_t batch = 0; batch < OPERATION_COUNT; batch += REOPEN_INTERVAL) {
        minidb::Pager pager(database.path().string());
        const bool afterReopen = batch != 0;
        if (!afterReopen) {
            recordPageId = pager.allocatePage();
        }
        auto tree = afterReopen
            ? minidb::PersistentBPlusTree::open(pager, metadataPageId)
            : minidb::PersistentBPlusTree::create(pager, leafCapacity, internalCapacity);
        if (!afterReopen) {
            metadataPageId = tree.metadataPageId();
        }
        requireMatches(tree, reference);

        const auto end = std::min(batch + REOPEN_INTERVAL, OPERATION_COUNT);
        for (std::size_t operation = batch; operation < end; ++operation) {
            const auto key = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
            const auto choice = random() % 100U;
            try {
                if (choice < 45U) {
                    const auto rid = makeRid(key, recordPageId, static_cast<std::uint32_t>(operation));
                    const auto persistentResult = tree.insert(key, rid);
                    const auto memoryResult = memory.insert(key, rid);
                    const auto referenceResult = reference.emplace(key, rid).second;
                    minidb::test::require(
                        persistentResult == referenceResult && memoryResult == referenceResult,
                        "Differential insert result differs");
                } else if (choice < 75U) {
                    const auto persistentResult = tree.erase(key);
                    const auto memoryResult = memory.erase(key);
                    const auto referenceResult = reference.erase(key) != 0;
                    minidb::test::require(
                        persistentResult == referenceResult && memoryResult == referenceResult,
                        "Differential erase result differs");
                } else if (choice < 88U) {
                    const auto expected = reference.find(key);
                    minidb::test::require(
                        tree.find(key)
                                == (expected == reference.end()
                                        ? std::optional<minidb::RecordId>{}
                                        : std::optional<minidb::RecordId>{expected->second})
                            && memory.find(key)
                                == (expected == reference.end()
                                        ? std::optional<minidb::RecordId>{}
                                        : std::optional<minidb::RecordId>{expected->second}),
                        "Differential exact lookup differs");
                } else {
                    const auto other = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
                    const auto expected = expectedRange(reference, key, other);
                    minidb::test::require(
                        tree.rangeScan(key, other) == expected
                            && memory.rangeScan(key, other) == expected,
                        "Differential range differs");
                }

                if (operation % 40 == 0) {
                    requireMatches(tree, reference);
                    memory.validate();
                    minidb::test::require(
                        tree.scanAll() == memory.scanAll(),
                        "Persistent and in-memory scans differ");
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    randomContext(
                        seed, operation, leafCapacity, internalCapacity, key, afterReopen)
                    + ": " + error.what());
            }
        }
        requireMatches(tree, reference);
        pager.flushAll();
    }
}

void testRandomizedDifferentialWithReopens() {
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> CONFIGURATIONS{{
        {3, 3}, {4, 4}, {5, 4}, {8, 6},
    }};
    constexpr std::array<std::uint32_t, 2> SEEDS{{0x4B200001U, 0x4B20C0DEU}};
    for (const auto& [leafCapacity, internalCapacity] : CONFIGURATIONS) {
        for (const auto seed : SEEDS) {
            runDifferential(leafCapacity, internalCapacity, seed);
        }
    }
}

} // namespace

int main() {
    try {
        testMissingSimpleAndFinalErase();
        testTargetedLeafBorrowMergeAndSeparatorRepair();
        testAdversarialEraseOrdersAndRebalancing();
        testReopenDeletionAndPageReuse();
        testIndependentIndexesShareAllocatorSafely();
        testDeletionEraCorruptionIsRejected();
        testRepeatedSplitMergeReuseCycles();
        testRandomizedDifferentialWithReopens();
        std::cout << "persistent_bplus_tree_delete_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent_bplus_tree_delete_test failed: " << error.what() << '\n';
        return 1;
    }
}
