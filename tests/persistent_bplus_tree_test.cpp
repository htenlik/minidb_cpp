#include "minidb/bplus_tree.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/record_page.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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

void writeUint16(
    minidb::Pager& pager,
    minidb::PageId pageId,
    std::size_t offset,
    std::uint16_t value) {
    auto& page = pager.getPage(pageId);
    for (std::size_t index = 0; index < 2; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    pager.markDirty(pageId);
}

void writeUint32(
    minidb::Pager& pager,
    minidb::PageId pageId,
    std::size_t offset,
    std::uint32_t value) {
    auto& page = pager.getPage(pageId);
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    pager.markDirty(pageId);
}

void writeUint64(
    minidb::Pager& pager,
    minidb::PageId pageId,
    std::size_t offset,
    std::uint64_t value) {
    auto& page = pager.getPage(pageId);
    for (std::size_t index = 0; index < 8; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    pager.markDirty(pageId);
}

std::uint32_t readUint32(
    const minidb::Pager::Page& page,
    std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint16_t readUint16(
    const minidb::Pager::Page& page,
    std::size_t offset) {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(page[offset + index]) << (index * 8U));
    }
    return value;
}

std::uint64_t readUint64(
    const minidb::Pager::Page& page,
    std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

std::vector<minidb::IndexEntry> expectedEntries(const ReferenceIndex& reference) {
    std::vector<minidb::IndexEntry> entries;
    entries.reserve(reference.size());
    for (const auto& [key, recordId] : reference) {
        entries.push_back(minidb::IndexEntry{key, recordId});
    }
    return entries;
}

std::vector<minidb::IndexEntry> expectedRange(
    const ReferenceIndex& reference,
    minidb::IndexKey lower,
    minidb::IndexKey upper) {
    std::vector<minidb::IndexEntry> entries;
    if (lower > upper) {
        return entries;
    }
    for (auto position = reference.lower_bound(lower);
         position != reference.end() && position->first <= upper;
         ++position) {
        entries.push_back(minidb::IndexEntry{position->first, position->second});
    }
    return entries;
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
    for (const auto& [key, recordId] : reference) {
        minidb::test::require(
            tree.find(key) == std::optional<minidb::RecordId>{recordId},
            "Persistent exact lookup differs from reference");
    }
}

void testPhysicalLayoutsAndEmptyMetadata() {
    static_assert(minidb::persistent_index_metadata_layout::HEADER_SIZE == 64);
    static_assert(minidb::persistent_index_metadata_layout::ROOT_PAGE_ID_OFFSET == 16);
    static_assert(minidb::persistent_index_metadata_layout::ENTRY_COUNT_OFFSET == 20);
    static_assert(minidb::persistent_bplus_leaf_layout::HEADER_SIZE == 32);
    static_assert(minidb::persistent_bplus_leaf_layout::ENTRY_SIZE == 10);
    static_assert(minidb::persistent_bplus_leaf_layout::PHYSICAL_CAPACITY == 406);
    static_assert(minidb::persistent_bplus_leaf_layout::UNUSED_SIZE == 4);
    static_assert(minidb::persistent_bplus_internal_layout::HEADER_SIZE == 32);
    static_assert(minidb::persistent_bplus_internal_layout::PHYSICAL_CAPACITY == 507);
    static_assert(minidb::persistent_bplus_internal_layout::PHYSICAL_FANOUT == 508);
    static_assert(minidb::persistent_bplus_internal_layout::UNUSED_SIZE == 4);

    minidb::test::TemporaryDatabase database("persistent_index_empty");
    minidb::Pager pager(database.path().string());
    auto tree = minidb::PersistentBPlusTree::create(pager);
    const auto metadataPageId = tree.metadataPageId();
    const auto& metadata = pager.getPage(metadataPageId);

    minidb::test::require(metadataPageId == 1, "Index metadata was not the first data page");
    minidb::test::require(tree.rootPageId() == minidb::INVALID_PAGE_ID, "Empty tree had a root");
    minidb::test::require(tree.size() == 0 && tree.empty(), "New persistent tree was not empty");
    minidb::test::require(tree.height() == 0, "Empty persistent tree had a height");
    minidb::test::require(!tree.find(7).has_value(), "Empty persistent tree found a key");
    minidb::test::require(tree.rangeScan(1, 9).empty(), "Empty persistent range was not empty");
    minidb::test::require(tree.scanAll().empty(), "Empty persistent full scan was not empty");
    minidb::test::require(
        std::equal(
            minidb::persistent_index_metadata_layout::MAGIC.begin(),
            minidb::persistent_index_metadata_layout::MAGIC.end(),
            metadata.begin()),
        "Index metadata magic was not encoded");
    minidb::test::require(
        readUint32(metadata, minidb::persistent_index_metadata_layout::LAYOUT_VERSION_OFFSET) == 1,
        "Index metadata version was not one");
    minidb::test::require(
        readUint32(metadata, minidb::persistent_index_metadata_layout::ROOT_PAGE_ID_OFFSET)
            == minidb::INVALID_PAGE_ID,
        "Index metadata root was not INVALID_PAGE_ID");
    minidb::test::require(
        readUint64(metadata, minidb::persistent_index_metadata_layout::ENTRY_COUNT_OFFSET) == 0,
        "Index metadata count was not zero");
    minidb::test::require(
        tree.leafMaxKeys() == minidb::PersistentBPlusTree::PHYSICAL_LEAF_MAX_KEYS,
        "Default leaf capacity was not physical capacity");
    minidb::test::require(
        tree.internalMaxKeys() == minidb::PersistentBPlusTree::PHYSICAL_INTERNAL_MAX_KEYS,
        "Default internal capacity was not physical capacity");
    tree.validate();

    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(minidb::PersistentBPlusTree::create(pager, 2, 3)); },
        "Persistent tree accepted leaf capacity below three");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] {
            static_cast<void>(minidb::PersistentBPlusTree::create(
                pager,
                minidb::PersistentBPlusTree::PHYSICAL_LEAF_MAX_KEYS + 1,
                3));
        },
        "Persistent tree accepted leaf capacity above physical capacity");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(minidb::PersistentBPlusTree::open(pager, 0)); },
        "Persistent tree accepted database metadata as index metadata");
}

void testRootGrowthLookupDuplicateAndRanges() {
    minidb::test::TemporaryDatabase database("persistent_index_root_growth");
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
    const auto original = makeRid(20, recordPageId, 1);

    minidb::test::require(tree.insert(20, original), "First persistent insert failed");
    const auto leafRoot = tree.rootPageId();
    const auto& leafBytes = pager.getPage(leafRoot);
    const auto entryOffset = minidb::persistent_bplus_leaf_layout::entryOffset(0);
    minidb::test::require(
        std::equal(
            minidb::persistent_bplus_leaf_layout::MAGIC.begin(),
            minidb::persistent_bplus_leaf_layout::MAGIC.end(),
            leafBytes.begin()),
        "Leaf magic was not encoded at offset zero");
    minidb::test::require(
        readUint32(leafBytes, minidb::persistent_bplus_leaf_layout::KEY_COUNT_OFFSET) == 1,
        "Leaf key count was not encoded");
    minidb::test::require(
        readUint32(leafBytes, entryOffset) == 20
            && readUint32(
                   leafBytes,
                   entryOffset + minidb::persistent_bplus_leaf_layout::KEY_SIZE)
                == original.pageId
            && readUint16(
                   leafBytes,
                   entryOffset + minidb::persistent_bplus_leaf_layout::KEY_SIZE
                       + minidb::persistent_bplus_leaf_layout::RECORD_PAGE_ID_SIZE)
                == original.slotId,
        "Leaf entry fields were not encoded at their explicit offsets");
    minidb::test::require(tree.height() == 1, "First insert did not create a leaf root");
    minidb::test::require(tree.find(20) == original, "First persistent RID was not found");
    minidb::test::require(
        !tree.insert(20, makeRid(20, recordPageId, 2)),
        "Persistent duplicate insert was accepted");
    minidb::test::require(tree.rootPageId() == leafRoot, "Duplicate insert changed root");
    minidb::test::require(tree.size() == 1, "Duplicate insert changed persistent size");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(tree.insert(99, minidb::INVALID_RECORD_ID)); },
        "Persistent tree accepted an invalid RID");

    for (const auto key : {0U, 10U}) {
        minidb::test::require(tree.insert(key, makeRid(key, recordPageId)), "Leaf insert failed");
    }
    minidb::test::require(tree.height() == 1, "Leaf split occurred before logical overflow");
    minidb::test::require(
        tree.insert(std::numeric_limits<minidb::IndexKey>::max(),
                    makeRid(std::numeric_limits<minidb::IndexKey>::max(), recordPageId)),
        "Maximum uint32 key insert failed");
    minidb::test::require(tree.height() == 2, "Leaf overflow did not create internal root");
    minidb::test::require(tree.rootPageId() != leafRoot, "Root split did not change root page");
    const auto& internalBytes = pager.getPage(tree.rootPageId());
    minidb::test::require(
        std::equal(
            minidb::persistent_bplus_internal_layout::MAGIC.begin(),
            minidb::persistent_bplus_internal_layout::MAGIC.end(),
            internalBytes.begin()),
        "Internal magic was not encoded at offset zero");
    minidb::test::require(
        readUint32(
            internalBytes,
            minidb::persistent_bplus_internal_layout::CHILD_COUNT_OFFSET)
            == 2,
        "Internal child count was not encoded");

    for (const auto key : {30U, 40U, 50U, 60U, 70U}) {
        static_cast<void>(tree.insert(key, makeRid(key, recordPageId)));
        tree.validate();
    }
    const std::vector<minidb::IndexEntry> expected{
        {10, makeRid(10, recordPageId)},
        {20, original},
        {30, makeRid(30, recordPageId)},
        {40, makeRid(40, recordPageId)},
    };
    minidb::test::require(
        tree.rangeScan(5, 45) == expected,
        "Inclusive persistent range with absent bounds was wrong");
    minidb::test::require(tree.rangeScan(50, 40).empty(), "Reversed persistent range was not empty");
    tree.validate();
}

void verifyInsertionOrder(std::vector<minidb::IndexKey> keys, std::string_view name) {
    minidb::test::TemporaryDatabase database(name);
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto persistent = minidb::PersistentBPlusTree::create(pager, 3, 3);
    minidb::BPlusTree memory(3, 3);
    ReferenceIndex reference;

    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto key = keys[index];
        const auto recordId = makeRid(key, recordPageId, 17);
        minidb::test::require(persistent.insert(key, recordId), "Persistent order insert failed");
        minidb::test::require(memory.insert(key, recordId), "4A order insert failed");
        reference.emplace(key, recordId);
        if (index % 7 == 0) {
            persistent.validate();
        }
    }
    requireMatches(persistent, reference);
    minidb::test::require(
        persistent.scanAll() == memory.scanAll(),
        "Persistent tree differs from 4A tree");
    minidb::test::require(persistent.height() >= 3, "Persistent tree did not reach three levels");
}

void testInsertionOrdersAndMultiLevelGrowth() {
    std::vector<minidb::IndexKey> keys(180);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        keys[index] = static_cast<minidb::IndexKey>(index);
    }
    verifyInsertionOrder(keys, "persistent_sorted");
    std::reverse(keys.begin(), keys.end());
    verifyInsertionOrder(keys, "persistent_reverse");
    std::mt19937 random(0x4B1001U);
    std::shuffle(keys.begin(), keys.end(), random);
    verifyInsertionOrder(keys, "persistent_random_order");
}

void testRepeatedCloseReopenAndContinuedSplitting() {
    minidb::test::TemporaryDatabase database("persistent_reopen_cycles");
    minidb::PageId metadataPageId = minidb::INVALID_PAGE_ID;
    minidb::PageId recordPageId = minidb::INVALID_PAGE_ID;
    ReferenceIndex reference;

    {
        minidb::Pager pager(database.path().string());
        recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 4);
        metadataPageId = tree.metadataPageId();
        for (minidb::IndexKey key = 0; key < 100; ++key) {
            const auto recordId = makeRid(key, recordPageId, 31);
            static_cast<void>(tree.insert(key, recordId));
            reference.emplace(key, recordId);
        }
        requireMatches(tree, reference);
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::open(pager, metadataPageId);
        minidb::test::require(tree.leafMaxKeys() == 3, "Leaf capacity did not persist");
        minidb::test::require(tree.internalMaxKeys() == 4, "Internal capacity did not persist");
        requireMatches(tree, reference);
        for (minidb::IndexKey key = 100; key < 240; ++key) {
            const auto recordId = makeRid(key, recordPageId, 31);
            static_cast<void>(tree.insert(key, recordId));
            reference.emplace(key, recordId);
        }
        requireMatches(tree, reference);
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::open(pager, metadataPageId);
        requireMatches(tree, reference);
        minidb::test::require(tree.height() >= 3, "Reopened tree lost its multi-level shape");
    }
}

template <typename Mutator>
void requireSingleLeafCorruption(std::string_view name, Mutator&& mutate) {
    minidb::test::TemporaryDatabase database(name);
    minidb::Pager pager(database.path().string());
    const auto recordPageId = pager.allocatePage();
    auto tree = minidb::PersistentBPlusTree::create(pager, 4, 4);
    for (const auto key : {10U, 20U, 30U}) {
        static_cast<void>(tree.insert(key, makeRid(key, recordPageId)));
    }
    mutate(pager, tree.rootPageId());
    minidb::test::requireThrows<std::runtime_error>(
        [&] { tree.validate(); },
        "Persistent tree accepted corrupted leaf state");
}

void testMetadataCorruption() {
    {
        minidb::test::TemporaryDatabase database("persistent_bad_meta_magic");
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        auto& page = pager.getPage(tree.metadataPageId());
        page[minidb::persistent_index_metadata_layout::MAGIC_OFFSET] = std::byte{'X'};
        pager.markDirty(tree.metadataPageId());
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(tree.size()); },
            "Persistent tree accepted invalid metadata magic");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_bad_meta_version");
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        writeUint32(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::LAYOUT_VERSION_OFFSET,
            minidb::persistent_index_metadata_layout::CURRENT_VERSION + 1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(tree.size()); },
            "Persistent tree accepted unsupported metadata version");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_bad_capacity");
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        writeUint32(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::LEAF_MAX_KEYS_OFFSET,
            2);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(tree.leafMaxKeys()); },
            "Persistent tree accepted invalid stored capacity");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_dangling_root");
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        writeUint32(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::ROOT_PAGE_ID_OFFSET,
            pager.pageCount() + 10);
        writeUint64(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::ENTRY_COUNT_OFFSET,
            1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] {
                static_cast<void>(
                    minidb::PersistentBPlusTree::open(pager, tree.metadataPageId()));
            },
            "Persistent tree accepted dangling root ID");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_wrong_root_type");
        minidb::Pager pager(database.path().string());
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        const auto recordPageId = pager.allocatePage();
        minidb::RecordPage::initialize(pager, recordPageId);
        writeUint32(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::ROOT_PAGE_ID_OFFSET,
            recordPageId);
        writeUint64(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::ENTRY_COUNT_OFFSET,
            1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] {
                static_cast<void>(
                    minidb::PersistentBPlusTree::open(pager, tree.metadataPageId()));
            },
            "Persistent tree accepted a RecordPage as its root");
    }
}

void testLeafCorruption() {
    requireSingleLeafCorruption("persistent_bad_leaf_magic", [](auto& pager, auto pageId) {
        auto& page = pager.getPage(pageId);
        page[minidb::persistent_bplus_leaf_layout::MAGIC_OFFSET] = std::byte{'X'};
        pager.markDirty(pageId);
    });
    requireSingleLeafCorruption("persistent_bad_leaf_version", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::persistent_bplus_leaf_layout::LAYOUT_VERSION_OFFSET,
            minidb::persistent_bplus_leaf_layout::CURRENT_VERSION + 1);
    });
    requireSingleLeafCorruption("persistent_excess_leaf_count", [](auto& pager, auto pageId) {
        writeUint32(pager, pageId, minidb::persistent_bplus_leaf_layout::KEY_COUNT_OFFSET, 5);
    });
    requireSingleLeafCorruption("persistent_unsorted_leaf", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::persistent_bplus_leaf_layout::entryOffset(1),
            10);
    });
    requireSingleLeafCorruption("persistent_invalid_leaf_rid", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::persistent_bplus_leaf_layout::entryOffset(0)
                + minidb::persistent_bplus_leaf_layout::KEY_SIZE,
            minidb::INVALID_PAGE_ID);
        writeUint16(
            pager,
            pageId,
            minidb::persistent_bplus_leaf_layout::entryOffset(0)
                + minidb::persistent_bplus_leaf_layout::KEY_SIZE
                + minidb::persistent_bplus_leaf_layout::RECORD_PAGE_ID_SIZE,
            minidb::INVALID_SLOT_ID);
    });
}

void testInternalAndChainCorruption() {
    const auto buildTwoLeafTree = [](minidb::Pager& pager) {
        const auto recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        for (minidb::IndexKey key = 0; key < 4; ++key) {
            static_cast<void>(tree.insert(key, makeRid(key, recordPageId)));
        }
        return tree;
    };
    {
        minidb::test::TemporaryDatabase database("persistent_dangling_child");
        minidb::Pager pager(database.path().string());
        auto tree = buildTwoLeafTree(pager);
        writeUint32(
            pager,
            tree.rootPageId(),
            minidb::persistent_bplus_internal_layout::childOffset(0),
            pager.pageCount() + 10);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Persistent tree accepted dangling internal child");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_bad_child_count");
        minidb::Pager pager(database.path().string());
        auto tree = buildTwoLeafTree(pager);
        writeUint32(
            pager,
            tree.rootPageId(),
            minidb::persistent_bplus_internal_layout::CHILD_COUNT_OFFSET,
            1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Persistent tree accepted malformed child/key counts");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_bad_separator");
        minidb::Pager pager(database.path().string());
        auto tree = buildTwoLeafTree(pager);
        const auto root = tree.rootPageId();
        const auto& page = pager.getPage(root);
        const auto separator = readUint32(page, minidb::persistent_bplus_internal_layout::keyOffset(0));
        writeUint32(
            pager,
            root,
            minidb::persistent_bplus_internal_layout::keyOffset(0),
            separator + 1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Persistent tree accepted incorrect separator");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_broken_sibling");
        minidb::Pager pager(database.path().string());
        auto tree = buildTwoLeafTree(pager);
        const auto& root = pager.getPage(tree.rootPageId());
        const auto left = readUint32(root, minidb::persistent_bplus_internal_layout::childOffset(0));
        writeUint32(
            pager,
            left,
            minidb::persistent_bplus_leaf_layout::NEXT_LEAF_PAGE_ID_OFFSET,
            minidb::INVALID_PAGE_ID);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Persistent tree accepted broken sibling link");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_leaf_cycle");
        minidb::Pager pager(database.path().string());
        const auto recordPageId = pager.allocatePage();
        auto tree = minidb::PersistentBPlusTree::create(pager, 3, 3);
        for (minidb::IndexKey key = 0; key < 7; ++key) {
            static_cast<void>(tree.insert(key, makeRid(key, recordPageId)));
        }
        const auto& root = pager.getPage(tree.rootPageId());
        const auto keyCount = readUint32(
            root,
            minidb::persistent_bplus_internal_layout::KEY_COUNT_OFFSET);
        const auto first = readUint32(
            root,
            minidb::persistent_bplus_internal_layout::childOffset(0));
        const auto last = readUint32(
            root,
            minidb::persistent_bplus_internal_layout::childOffset(keyCount));
        writeUint32(
            pager,
            last,
            minidb::persistent_bplus_leaf_layout::NEXT_LEAF_PAGE_ID_OFFSET,
            first);
        writeUint32(
            pager,
            first,
            minidb::persistent_bplus_leaf_layout::PREVIOUS_LEAF_PAGE_ID_OFFSET,
            last);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Persistent tree accepted a cyclic leaf chain");
    }
    {
        minidb::test::TemporaryDatabase database("persistent_size_mismatch");
        minidb::Pager pager(database.path().string());
        auto tree = buildTwoLeafTree(pager);
        writeUint64(
            pager,
            tree.metadataPageId(),
            minidb::persistent_index_metadata_layout::ENTRY_COUNT_OFFSET,
            tree.size() + 1);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { tree.validate(); },
            "Persistent tree accepted metadata size disagreement");
    }
}

std::string randomContext(
    std::uint32_t seed,
    std::size_t operation,
    std::size_t leafCapacity,
    std::size_t internalCapacity,
    minidb::IndexKey key,
    bool afterReopen) {
    std::ostringstream stream;
    stream << "seed=" << seed << " operation=" << operation
           << " capacities=" << leafCapacity << '/' << internalCapacity
           << " key=" << key << " after_reopen=" << std::boolalpha << afterReopen;
    return stream.str();
}

void runPersistentDifferential(
    std::uint32_t leafCapacity,
    std::uint32_t internalCapacity,
    std::uint32_t seed) {
    constexpr std::size_t OPERATION_COUNT = 1'000;
    constexpr std::size_t REOPEN_INTERVAL = 100;
    constexpr minidb::IndexKey KEY_DOMAIN = 1'024;
    minidb::test::TemporaryDatabase database("persistent_differential");
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
                if (choice < 55U) {
                    const auto recordId =
                        makeRid(key, recordPageId, static_cast<std::uint32_t>(operation));
                    const bool persistentResult = tree.insert(key, recordId);
                    const bool memoryResult = memory.insert(key, recordId);
                    const bool referenceResult = reference.emplace(key, recordId).second;
                    minidb::test::require(
                        persistentResult == referenceResult && memoryResult == referenceResult,
                        "Differential insert results disagree");
                } else if (choice < 80U) {
                    const auto expected = reference.find(key);
                    const auto persistentResult = tree.find(key);
                    const auto memoryResult = memory.find(key);
                    minidb::test::require(
                        persistentResult.has_value() == (expected != reference.end())
                            && memoryResult.has_value() == (expected != reference.end()),
                        "Differential find presence disagrees");
                    if (persistentResult) {
                        minidb::test::require(
                            *persistentResult == expected->second
                                && *memoryResult == expected->second,
                            "Differential find RID disagrees");
                    }
                } else {
                    const auto other = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
                    const auto expected = expectedRange(reference, key, other);
                    minidb::test::require(
                        tree.rangeScan(key, other) == expected
                            && memory.rangeScan(key, other) == expected,
                        "Differential range scan disagrees");
                }

                if (operation % 25 == 0) {
                    requireMatches(tree, reference);
                    memory.validate();
                    minidb::test::require(
                        tree.scanAll() == memory.scanAll(),
                        "Persistent and 4A full scans disagree");
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    randomContext(
                        seed,
                        operation,
                        leafCapacity,
                        internalCapacity,
                        key,
                        afterReopen)
                    + ": " + error.what());
            }
        }
        requireMatches(tree, reference);
        pager.flushAll();
    }

    minidb::Pager pager(database.path().string());
    auto tree = minidb::PersistentBPlusTree::open(pager, metadataPageId);
    requireMatches(tree, reference);
    minidb::test::require(tree.scanAll() == memory.scanAll(), "Final differential scan differs");
}

void testRandomizedDifferentialReopenWorkloads() {
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 3> CONFIGURATIONS{{
        {3, 3}, {4, 4}, {5, 4},
    }};
    constexpr std::array<std::uint32_t, 2> SEEDS{{0x4B100001U, 0x4B10C0DEU}};
    for (const auto& [leafCapacity, internalCapacity] : CONFIGURATIONS) {
        for (const auto seed : SEEDS) {
            runPersistentDifferential(leafCapacity, internalCapacity, seed);
        }
    }
}

} // namespace

int main() {
    try {
        testPhysicalLayoutsAndEmptyMetadata();
        testRootGrowthLookupDuplicateAndRanges();
        testInsertionOrdersAndMultiLevelGrowth();
        testRepeatedCloseReopenAndContinuedSplitting();
        testMetadataCorruption();
        testLeafCorruption();
        testInternalAndChainCorruption();
        testRandomizedDifferentialReopenWorkloads();
        std::cout << "persistent_bplus_tree_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent_bplus_tree_test failed: " << error.what() << '\n';
        return 1;
    }
}
