#pragma once

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/disk_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/slotted_page.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace minidb {

namespace tuple_heap_metadata_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'H'},
    std::byte{'P'}, std::byte{'M'}, std::byte{'E'}, std::byte{'T'},
};
inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t HEADER_SIZE_OFFSET = LAYOUT_VERSION_OFFSET + 4;
inline constexpr std::size_t FIRST_PAGE_ID_OFFSET = HEADER_SIZE_OFFSET + 4;
inline constexpr std::size_t LAST_PAGE_ID_OFFSET = FIRST_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t TUPLE_COUNT_OFFSET = LAST_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t RESERVED_OFFSET = TUPLE_COUNT_OFFSET + 8;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t RESERVED_SIZE = HEADER_SIZE - RESERVED_OFFSET;

static_assert(RESERVED_OFFSET == 32);
static_assert(HEADER_SIZE <= database_format::PAGE_SIZE);

} // namespace tuple_heap_metadata_layout

class TupleStore {
public:
    using ScanEntry = std::pair<RecordId, TupleBytes>;

    [[nodiscard]] static TupleStore create(
        BufferPoolManager& bufferPool,
        DiskManager& diskManager,
        PageAllocator& allocator);
    [[nodiscard]] static TupleStore open(
        BufferPoolManager& bufferPool,
        DiskManager& diskManager,
        PageAllocator& allocator,
        PageId metadataPageId);

    TupleStore(const TupleStore&) = delete;
    TupleStore& operator=(const TupleStore&) = delete;
    TupleStore(TupleStore&&) noexcept = default;
    TupleStore& operator=(TupleStore&&) = delete;

    [[nodiscard]] PageId metadataPageId() const noexcept { return metadataPageId_; }
    [[nodiscard]] PageId firstPageId() const;
    [[nodiscard]] PageId lastPageId() const;
    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] bool empty() const;

    [[nodiscard]] RecordId insert(std::span<const std::byte> tuple);
    [[nodiscard]] TupleBytes get(RecordId recordId) const;
    [[nodiscard]] bool tryUpdate(RecordId recordId, std::span<const std::byte> tuple);
    void erase(RecordId recordId);
    [[nodiscard]] std::vector<ScanEntry> scan() const;
    [[nodiscard]] std::vector<PageId> reachablePageIds() const;
    void validate() const;

private:
    struct Metadata {
        PageId firstPageId = INVALID_PAGE_ID;
        PageId lastPageId = INVALID_PAGE_ID;
        std::uint64_t tupleCount = 0;
    };

    BufferPoolManager& bufferPool_;
    DiskManager& diskManager_;
    PageAllocator& allocator_;
    PageId metadataPageId_;

    TupleStore(
        BufferPoolManager& bufferPool,
        DiskManager& diskManager,
        PageAllocator& allocator,
        PageId metadataPageId)
        : bufferPool_(bufferPool),
          diskManager_(diskManager),
          allocator_(allocator),
          metadataPageId_(metadataPageId) {}

    [[nodiscard]] Metadata readMetadata() const;
    void writeMetadata(const Metadata& metadata);
    void validateMetadataPageId() const;
    static void validateTupleSize(std::size_t size);
    void validateOwnedPage(const ConstSlottedPageView& page) const;
    void validateRecordId(RecordId recordId, const ConstSlottedPageView& page) const;
};

} // namespace minidb
