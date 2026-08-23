#pragma once

#include "minidb/index_types.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/pager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace minidb {

namespace persistent_index_metadata_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'I'},
    std::byte{'D'}, std::byte{'X'}, std::byte{'M'}, std::byte{'D'},
};
inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t HEADER_SIZE_OFFSET = LAYOUT_VERSION_OFFSET + 4;
inline constexpr std::size_t ROOT_PAGE_ID_OFFSET = HEADER_SIZE_OFFSET + 4;
inline constexpr std::size_t ENTRY_COUNT_OFFSET = ROOT_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t LEAF_MAX_KEYS_OFFSET = ENTRY_COUNT_OFFSET + 8;
inline constexpr std::size_t INTERNAL_MAX_KEYS_OFFSET = LEAF_MAX_KEYS_OFFSET + 4;
inline constexpr std::size_t RESERVED_OFFSET = INTERNAL_MAX_KEYS_OFFSET + 4;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t RESERVED_SIZE = HEADER_SIZE - RESERVED_OFFSET;

static_assert(RESERVED_OFFSET == 36);
static_assert(HEADER_SIZE <= Pager::PAGE_SIZE);

} // namespace persistent_index_metadata_layout

namespace persistent_bplus_leaf_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'I'},
    std::byte{'D'}, std::byte{'X'}, std::byte{'L'}, std::byte{'F'},
};
inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t HEADER_SIZE_OFFSET = LAYOUT_VERSION_OFFSET + 4;
inline constexpr std::size_t KEY_COUNT_OFFSET = HEADER_SIZE_OFFSET + 4;
inline constexpr std::size_t NEXT_LEAF_PAGE_ID_OFFSET = KEY_COUNT_OFFSET + 4;
inline constexpr std::size_t PREVIOUS_LEAF_PAGE_ID_OFFSET =
    NEXT_LEAF_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t RESERVED_OFFSET = PREVIOUS_LEAF_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t RESERVED_SIZE = 4;
inline constexpr std::size_t HEADER_SIZE = RESERVED_OFFSET + RESERVED_SIZE;

inline constexpr std::size_t KEY_SIZE = 4;
inline constexpr std::size_t RECORD_PAGE_ID_SIZE = 4;
inline constexpr std::size_t SLOT_ID_SIZE = 2;
inline constexpr std::size_t ENTRY_SIZE = KEY_SIZE + RECORD_PAGE_ID_SIZE + SLOT_ID_SIZE;
inline constexpr std::size_t ENTRY_DATA_OFFSET = HEADER_SIZE;
inline constexpr std::size_t PHYSICAL_CAPACITY =
    (Pager::PAGE_SIZE - ENTRY_DATA_OFFSET) / ENTRY_SIZE;
inline constexpr std::size_t USED_SIZE =
    ENTRY_DATA_OFFSET + (PHYSICAL_CAPACITY * ENTRY_SIZE);
inline constexpr std::size_t UNUSED_SIZE = Pager::PAGE_SIZE - USED_SIZE;

[[nodiscard]] constexpr std::size_t entryOffset(std::size_t index) noexcept {
    return ENTRY_DATA_OFFSET + (index * ENTRY_SIZE);
}

static_assert(HEADER_SIZE == 32);
static_assert(ENTRY_SIZE == 10);
static_assert(PHYSICAL_CAPACITY == 406);
static_assert(USED_SIZE <= Pager::PAGE_SIZE);
static_assert(USED_SIZE + ENTRY_SIZE > Pager::PAGE_SIZE);

} // namespace persistent_bplus_leaf_layout

namespace persistent_bplus_internal_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'I'},
    std::byte{'D'}, std::byte{'X'}, std::byte{'I'}, std::byte{'N'},
};
inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t HEADER_SIZE_OFFSET = LAYOUT_VERSION_OFFSET + 4;
inline constexpr std::size_t KEY_COUNT_OFFSET = HEADER_SIZE_OFFSET + 4;
inline constexpr std::size_t CHILD_COUNT_OFFSET = KEY_COUNT_OFFSET + 4;
inline constexpr std::size_t RESERVED_OFFSET = CHILD_COUNT_OFFSET + 4;
inline constexpr std::size_t RESERVED_SIZE = 8;
inline constexpr std::size_t HEADER_SIZE = RESERVED_OFFSET + RESERVED_SIZE;

inline constexpr std::size_t PAGE_ID_SIZE = 4;
inline constexpr std::size_t KEY_SIZE = 4;
inline constexpr std::size_t KEY_CHILD_PAIR_SIZE = KEY_SIZE + PAGE_ID_SIZE;
inline constexpr std::size_t FIRST_CHILD_OFFSET = HEADER_SIZE;
inline constexpr std::size_t PHYSICAL_CAPACITY =
    (Pager::PAGE_SIZE - HEADER_SIZE - PAGE_ID_SIZE) / KEY_CHILD_PAIR_SIZE;
inline constexpr std::size_t PHYSICAL_FANOUT = PHYSICAL_CAPACITY + 1;
inline constexpr std::size_t USED_SIZE =
    HEADER_SIZE + PAGE_ID_SIZE + (PHYSICAL_CAPACITY * KEY_CHILD_PAIR_SIZE);
inline constexpr std::size_t UNUSED_SIZE = Pager::PAGE_SIZE - USED_SIZE;

[[nodiscard]] constexpr std::size_t childOffset(std::size_t index) noexcept {
    return FIRST_CHILD_OFFSET + (index * KEY_CHILD_PAIR_SIZE);
}

[[nodiscard]] constexpr std::size_t keyOffset(std::size_t index) noexcept {
    return FIRST_CHILD_OFFSET + PAGE_ID_SIZE + (index * KEY_CHILD_PAIR_SIZE);
}

static_assert(HEADER_SIZE == 32);
static_assert(PHYSICAL_CAPACITY == 507);
static_assert(PHYSICAL_FANOUT == 508);
static_assert(USED_SIZE <= Pager::PAGE_SIZE);
static_assert(USED_SIZE + KEY_CHILD_PAIR_SIZE > Pager::PAGE_SIZE);

} // namespace persistent_bplus_internal_layout

class PersistentBPlusTree {
public:
    static constexpr std::uint32_t MIN_LOGICAL_MAX_KEYS = 3;
    static constexpr std::uint32_t PHYSICAL_LEAF_MAX_KEYS =
        static_cast<std::uint32_t>(persistent_bplus_leaf_layout::PHYSICAL_CAPACITY);
    static constexpr std::uint32_t PHYSICAL_INTERNAL_MAX_KEYS =
        static_cast<std::uint32_t>(persistent_bplus_internal_layout::PHYSICAL_CAPACITY);

    [[nodiscard]] static PersistentBPlusTree create(
        Pager& pager,
        std::uint32_t leafMaxKeys = PHYSICAL_LEAF_MAX_KEYS,
        std::uint32_t internalMaxKeys = PHYSICAL_INTERNAL_MAX_KEYS);
    [[nodiscard]] static PersistentBPlusTree open(Pager& pager, PageId metadataPageId);

    PersistentBPlusTree(const PersistentBPlusTree&) = delete;
    PersistentBPlusTree& operator=(const PersistentBPlusTree&) = delete;
    PersistentBPlusTree(PersistentBPlusTree&&) noexcept = default;
    PersistentBPlusTree& operator=(PersistentBPlusTree&&) = delete;

    [[nodiscard]] PageId metadataPageId() const noexcept { return metadataPageId_; }
    [[nodiscard]] PageId rootPageId() const;
    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::uint32_t leafMaxKeys() const;
    [[nodiscard]] std::uint32_t internalMaxKeys() const;
    [[nodiscard]] std::size_t height() const;

    [[nodiscard]] bool insert(IndexKey key, RecordId recordId);
    [[nodiscard]] bool erase(IndexKey key);
    [[nodiscard]] std::optional<RecordId> find(IndexKey key) const;
    [[nodiscard]] std::vector<IndexEntry> rangeScan(
        IndexKey lowerInclusive,
        IndexKey upperInclusive) const;
    [[nodiscard]] std::vector<IndexEntry> scanAll() const;
    [[nodiscard]] std::vector<PageId> reachableNodePageIds() const;

    void validate() const;

private:
    struct Metadata;
    struct LeafNode;
    struct InternalNode;
    struct PathFrame;
    struct SplitResult;

    Pager& pager_;
    PageAllocator allocator_;
    PageId metadataPageId_;

    PersistentBPlusTree(Pager& pager, PageId metadataPageId)
        : pager_(pager), allocator_(pager), metadataPageId_(metadataPageId) {}

    [[nodiscard]] Metadata readMetadata() const;
    void writeMetadata(const Metadata& metadata);
    void validateMetadataPageId() const;

    [[nodiscard]] LeafNode readLeaf(
        PageId pageId,
        std::uint32_t logicalCapacity) const;
    void writeLeaf(PageId pageId, const LeafNode& leaf);
    [[nodiscard]] InternalNode readInternal(
        PageId pageId,
        std::uint32_t logicalCapacity) const;
    void writeInternal(PageId pageId, const InternalNode& internalNode);
    [[nodiscard]] bool isLeafPage(PageId pageId) const;

    [[nodiscard]] PageId findLeafPage(
        IndexKey key,
        const Metadata& metadata,
        std::vector<PathFrame>* path) const;
    [[nodiscard]] IndexKey subtreeMinimum(PageId pageId, const Metadata& metadata) const;
    [[nodiscard]] PageId allocateLeaf(const LeafNode& leaf);
    [[nodiscard]] PageId allocateInternal(const InternalNode& internalNode);
    [[nodiscard]] SplitResult splitLeaf(
        PageId pageId,
        LeafNode leaf,
        const Metadata& metadata);
    [[nodiscard]] SplitResult splitInternal(PageId pageId, InternalNode internalNode);

    [[nodiscard]] static std::size_t minimumLeafKeys(const Metadata& metadata) noexcept;
    [[nodiscard]] static std::size_t minimumInternalChildren(
        const Metadata& metadata) noexcept;
    void rebuildInternalKeys(InternalNode& internalNode, const Metadata& metadata) const;
    void refreshAncestorSeparators(
        const std::vector<PathFrame>& path,
        const Metadata& metadata);
    void rebalanceLeafAfterErase(
        PageId pageId,
        LeafNode leaf,
        std::vector<PathFrame> path,
        Metadata& metadata);
    void repairInternalAfterChildRemoval(
        PageId pageId,
        InternalNode internalNode,
        std::vector<PathFrame> path,
        Metadata& metadata);
};

} // namespace minidb
