#pragma once

#include "minidb/pager.hpp"
#include "minidb/record_id.hpp"
#include "minidb/row.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace minidb {

namespace record_page_layout {

using LayoutVersion = std::uint32_t;

inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'},
    std::byte{'D'},
    std::byte{'B'},
    std::byte{'R'},
    std::byte{'E'},
    std::byte{'C'},
    std::byte{'P'},
    std::byte{'G'},
};

inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t UINT32_FIELD_SIZE = 4;
inline constexpr std::size_t HEADER_SIZE_OFFSET =
    LAYOUT_VERSION_OFFSET + UINT32_FIELD_SIZE;
inline constexpr std::size_t NEXT_PAGE_ID_OFFSET = HEADER_SIZE_OFFSET + UINT32_FIELD_SIZE;
inline constexpr std::size_t LIVE_COUNT_OFFSET = NEXT_PAGE_ID_OFFSET + UINT32_FIELD_SIZE;
inline constexpr std::size_t UINT16_FIELD_SIZE = 2;
inline constexpr std::size_t SLOT_CAPACITY_OFFSET = LIVE_COUNT_OFFSET + UINT16_FIELD_SIZE;
inline constexpr std::size_t SLOT_SIZE_OFFSET = SLOT_CAPACITY_OFFSET + UINT16_FIELD_SIZE;
inline constexpr std::size_t RESERVED_OFFSET = SLOT_SIZE_OFFSET + UINT32_FIELD_SIZE;
inline constexpr std::size_t RESERVED_SIZE = 4;
inline constexpr std::size_t HEADER_SIZE = RESERVED_OFFSET + RESERVED_SIZE;

[[nodiscard]] constexpr std::size_t occupancySizeFor(std::size_t slotCount) noexcept {
    constexpr std::size_t BITS_PER_BYTE = 8;
    return (slotCount + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
}

[[nodiscard]] constexpr std::size_t calculateSlotCapacity() noexcept {
    std::size_t capacity = 0;
    while (HEADER_SIZE + occupancySizeFor(capacity + 1)
               + ((capacity + 1) * row_layout::SERIALIZED_SIZE)
           <= Pager::PAGE_SIZE) {
        ++capacity;
    }
    return capacity;
}

inline constexpr std::size_t SLOT_CAPACITY = calculateSlotCapacity();
inline constexpr std::size_t OCCUPANCY_OFFSET = HEADER_SIZE;
inline constexpr std::size_t OCCUPANCY_SIZE = occupancySizeFor(SLOT_CAPACITY);
inline constexpr std::size_t SLOT_DATA_OFFSET = OCCUPANCY_OFFSET + OCCUPANCY_SIZE;
inline constexpr std::size_t USED_SIZE =
    SLOT_DATA_OFFSET + (SLOT_CAPACITY * row_layout::SERIALIZED_SIZE);
inline constexpr std::size_t UNUSED_SIZE = Pager::PAGE_SIZE - USED_SIZE;

[[nodiscard]] constexpr std::size_t slotOffset(SlotId slotId) noexcept {
    return SLOT_DATA_OFFSET
        + (static_cast<std::size_t>(slotId) * row_layout::SERIALIZED_SIZE);
}

static_assert(SLOT_CAPACITY > 0);
static_assert(SLOT_CAPACITY < INVALID_SLOT_ID);
static_assert(USED_SIZE <= Pager::PAGE_SIZE);
static_assert(
    HEADER_SIZE + occupancySizeFor(SLOT_CAPACITY + 1)
        + ((SLOT_CAPACITY + 1) * row_layout::SERIALIZED_SIZE)
    > Pager::PAGE_SIZE);

} // namespace record_page_layout

class RecordPage {
public:
    static void initialize(Pager& pager, PageId pageId);

    RecordPage(Pager& pager, PageId pageId);

    [[nodiscard]] PageId pageId() const noexcept { return pageId_; }
    [[nodiscard]] PageId nextPageId() const noexcept;
    void setNextPageId(PageId nextPageId);

    [[nodiscard]] SlotId liveCount() const noexcept;
    [[nodiscard]] bool hasFreeSlot() const noexcept;
    [[nodiscard]] bool isOccupied(SlotId slotId) const;

    [[nodiscard]] SlotId insert(const SerializedRow& row);
    [[nodiscard]] SerializedRow get(SlotId slotId) const;
    void update(SlotId slotId, const SerializedRow& row);
    void erase(SlotId slotId);

private:
    Pager& pager_;
    PageId pageId_;
    Pager::Page& bytes_;

    void validate() const;
    void validateNextPageId(PageId nextPageId) const;
    static void validateSlotId(SlotId slotId);
    [[nodiscard]] bool isOccupiedUnchecked(std::size_t slotIndex) const noexcept;
    void setOccupied(SlotId slotId, bool occupied) noexcept;
};

} // namespace minidb
