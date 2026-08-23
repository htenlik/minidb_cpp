#pragma once

#include "minidb/pager.hpp"
#include "minidb/record_id.hpp"
#include "minidb/tuple_bytes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace minidb {

namespace slotted_page_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'S'},
    std::byte{'L'}, std::byte{'T'}, std::byte{'P'}, std::byte{'G'},
};
inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t HEADER_SIZE_OFFSET = LAYOUT_VERSION_OFFSET + 4;
inline constexpr std::size_t SLOT_COUNT_OFFSET = HEADER_SIZE_OFFSET + 4;
inline constexpr std::size_t LIVE_COUNT_OFFSET = SLOT_COUNT_OFFSET + 2;
inline constexpr std::size_t LOWER_BOUNDARY_OFFSET = LIVE_COUNT_OFFSET + 2;
inline constexpr std::size_t UPPER_BOUNDARY_OFFSET = LOWER_BOUNDARY_OFFSET + 2;
inline constexpr std::size_t NEXT_PAGE_ID_OFFSET = UPPER_BOUNDARY_OFFSET + 2;
inline constexpr std::size_t PREVIOUS_PAGE_ID_OFFSET = NEXT_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t HEAP_METADATA_PAGE_ID_OFFSET = PREVIOUS_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t RESERVED_OFFSET = HEAP_METADATA_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t RESERVED_SIZE = 12;
inline constexpr std::size_t HEADER_SIZE = RESERVED_OFFSET + RESERVED_SIZE;

inline constexpr std::size_t SLOT_TUPLE_OFFSET_OFFSET = 0;
inline constexpr std::size_t SLOT_TUPLE_LENGTH_OFFSET = SLOT_TUPLE_OFFSET_OFFSET + 2;
inline constexpr std::size_t SLOT_FLAGS_OFFSET = SLOT_TUPLE_LENGTH_OFFSET + 2;
inline constexpr std::size_t SLOT_RESERVED_OFFSET = SLOT_FLAGS_OFFSET + 2;
inline constexpr std::size_t SLOT_SIZE = SLOT_RESERVED_OFFSET + 2;
inline constexpr std::uint16_t SLOT_FREE = 0;
inline constexpr std::uint16_t SLOT_LIVE = 1;

inline constexpr std::size_t MAX_SLOT_COUNT =
    (Pager::PAGE_SIZE - HEADER_SIZE) / SLOT_SIZE;
inline constexpr std::size_t MAX_TUPLE_SIZE =
    Pager::PAGE_SIZE - HEADER_SIZE - SLOT_SIZE;

[[nodiscard]] constexpr std::size_t slotOffset(SlotId slotId) noexcept {
    return Pager::PAGE_SIZE
        - ((static_cast<std::size_t>(slotId) + 1) * SLOT_SIZE);
}

static_assert(HEADER_SIZE == 48);
static_assert(SLOT_SIZE == 8);
static_assert(MAX_SLOT_COUNT == 506);
static_assert(MAX_SLOT_COUNT < INVALID_SLOT_ID);
static_assert(MAX_TUPLE_SIZE == 4040);

} // namespace slotted_page_layout

class SlottedPage {
public:
    static void initialize(
        Pager& pager,
        PageId pageId,
        PageId heapMetadataPageId,
        PageId nextPageId = INVALID_PAGE_ID,
        PageId previousPageId = INVALID_PAGE_ID);

    SlottedPage(Pager& pager, PageId pageId);

    [[nodiscard]] PageId pageId() const noexcept { return pageId_; }
    [[nodiscard]] PageId heapMetadataPageId() const noexcept;
    [[nodiscard]] PageId nextPageId() const noexcept;
    [[nodiscard]] PageId previousPageId() const noexcept;
    void setNextPageId(PageId pageId);
    void setPreviousPageId(PageId pageId);

    [[nodiscard]] std::uint16_t slotCount() const noexcept;
    [[nodiscard]] std::uint16_t liveCount() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return liveCount() == 0; }
    [[nodiscard]] std::uint16_t lowerBoundary() const noexcept;
    [[nodiscard]] std::uint16_t upperBoundary() const noexcept;
    [[nodiscard]] std::size_t freeSpace() const noexcept;
    [[nodiscard]] bool canFit(std::size_t tupleSize) const noexcept;
    [[nodiscard]] bool isOccupied(SlotId slotId) const;

    [[nodiscard]] SlotId insert(std::span<const std::byte> tuple);
    [[nodiscard]] TupleBytes get(SlotId slotId) const;
    [[nodiscard]] bool tryUpdate(SlotId slotId, std::span<const std::byte> tuple);
    void erase(SlotId slotId);
    void compact();
    void validate() const;

private:
    struct SlotEntry {
        std::uint16_t tupleOffset;
        std::uint16_t tupleLength;
        std::uint16_t flags;
        std::uint16_t reserved;
    };

    Pager& pager_;
    PageId pageId_;
    Pager::Page& bytes_;

    [[nodiscard]] SlotEntry readSlot(SlotId slotId) const noexcept;
    void writeSlot(SlotId slotId, const SlotEntry& slot) noexcept;
    void validateSlotId(SlotId slotId) const;
    void validateLink(PageId linkedPageId) const;
    static void validateTupleSize(std::size_t tupleSize);
    [[nodiscard]] std::vector<std::pair<SlotId, TupleBytes>> snapshotLiveTuples() const;
    void rewritePayload(const std::vector<std::pair<SlotId, TupleBytes>>& tuples);
};

} // namespace minidb
