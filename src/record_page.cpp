#include "minidb/record_page.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace minidb {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

void writeUint16LittleEndian(
    Pager::Page& output,
    std::size_t offset,
    std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < record_page_layout::UINT16_FIELD_SIZE; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

void writeUint32LittleEndian(
    Pager::Page& output,
    std::size_t offset,
    std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < record_page_layout::UINT32_FIELD_SIZE; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

std::uint16_t readUint16LittleEndian(
    const Pager::Page& input,
    std::size_t offset) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < record_page_layout::UINT16_FIELD_SIZE; ++index) {
        value |= static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(input[offset + index])
            << (index * BITS_PER_BYTE));
    }
    return value;
}

std::uint32_t readUint32LittleEndian(
    const Pager::Page& input,
    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < record_page_layout::UINT32_FIELD_SIZE; ++index) {
        value |= std::to_integer<std::uint32_t>(input[offset + index])
            << (index * BITS_PER_BYTE);
    }
    return value;
}

} // namespace

void RecordPage::initialize(Pager& pager, PageId pageId) {
    auto& page = pager.getPage(pageId);
    page.fill(std::byte{0});

    std::copy(
        record_page_layout::MAGIC.begin(),
        record_page_layout::MAGIC.end(),
        page.begin() + record_page_layout::MAGIC_OFFSET);
    writeUint32LittleEndian(
        page,
        record_page_layout::LAYOUT_VERSION_OFFSET,
        record_page_layout::CURRENT_VERSION);
    writeUint32LittleEndian(
        page,
        record_page_layout::HEADER_SIZE_OFFSET,
        static_cast<std::uint32_t>(record_page_layout::HEADER_SIZE));
    writeUint32LittleEndian(
        page,
        record_page_layout::NEXT_PAGE_ID_OFFSET,
        INVALID_PAGE_ID);
    writeUint16LittleEndian(page, record_page_layout::LIVE_COUNT_OFFSET, 0);
    writeUint16LittleEndian(
        page,
        record_page_layout::SLOT_CAPACITY_OFFSET,
        static_cast<std::uint16_t>(record_page_layout::SLOT_CAPACITY));
    writeUint32LittleEndian(
        page,
        record_page_layout::SLOT_SIZE_OFFSET,
        static_cast<std::uint32_t>(row_layout::SERIALIZED_SIZE));

    pager.markDirty(pageId);
}

RecordPage::RecordPage(Pager& pager, PageId pageId)
    : pager_(pager), pageId_(pageId), bytes_(pager.getPage(pageId)) {
    validate();
}

PageId RecordPage::nextPageId() const noexcept {
    return readUint32LittleEndian(bytes_, record_page_layout::NEXT_PAGE_ID_OFFSET);
}

void RecordPage::setNextPageId(PageId nextPageId) {
    validateNextPageId(nextPageId);
    writeUint32LittleEndian(bytes_, record_page_layout::NEXT_PAGE_ID_OFFSET, nextPageId);
    pager_.markDirty(pageId_);
}

SlotId RecordPage::liveCount() const noexcept {
    return readUint16LittleEndian(bytes_, record_page_layout::LIVE_COUNT_OFFSET);
}

bool RecordPage::hasFreeSlot() const noexcept {
    return liveCount() < record_page_layout::SLOT_CAPACITY;
}

bool RecordPage::isOccupied(SlotId slotId) const {
    validateSlotId(slotId);
    return isOccupiedUnchecked(slotId);
}

SlotId RecordPage::insert(const SerializedRow& row) {
    if (!hasFreeSlot()) {
        throw std::overflow_error("Record page has no free slots.");
    }

    for (std::size_t index = 0; index < record_page_layout::SLOT_CAPACITY; ++index) {
        if (isOccupiedUnchecked(index)) {
            continue;
        }

        const auto slotId = static_cast<SlotId>(index);
        std::copy(
            row.begin(),
            row.end(),
            bytes_.begin() + record_page_layout::slotOffset(slotId));
        setOccupied(slotId, true);
        writeUint16LittleEndian(
            bytes_,
            record_page_layout::LIVE_COUNT_OFFSET,
            static_cast<std::uint16_t>(liveCount() + 1));
        pager_.markDirty(pageId_);
        return slotId;
    }

    throw std::runtime_error("Record page occupancy metadata is inconsistent.");
}

SerializedRow RecordPage::get(SlotId slotId) const {
    validateSlotId(slotId);
    if (!isOccupiedUnchecked(slotId)) {
        throw std::runtime_error("Record slot is not occupied.");
    }

    SerializedRow row{};
    const auto offset = record_page_layout::slotOffset(slotId);
    std::copy_n(bytes_.begin() + offset, row.size(), row.begin());
    return row;
}

void RecordPage::update(SlotId slotId, const SerializedRow& row) {
    validateSlotId(slotId);
    if (!isOccupiedUnchecked(slotId)) {
        throw std::runtime_error("Record slot is not occupied.");
    }

    std::copy(
        row.begin(),
        row.end(),
        bytes_.begin() + record_page_layout::slotOffset(slotId));
    pager_.markDirty(pageId_);
}

void RecordPage::erase(SlotId slotId) {
    validateSlotId(slotId);
    if (!isOccupiedUnchecked(slotId)) {
        throw std::runtime_error("Record slot is not occupied.");
    }

    const auto offset = record_page_layout::slotOffset(slotId);
    std::fill_n(bytes_.begin() + offset, row_layout::SERIALIZED_SIZE, std::byte{0});
    setOccupied(slotId, false);
    writeUint16LittleEndian(
        bytes_,
        record_page_layout::LIVE_COUNT_OFFSET,
        static_cast<std::uint16_t>(liveCount() - 1));
    pager_.markDirty(pageId_);
}

void RecordPage::validate() const {
    if (!std::equal(
            record_page_layout::MAGIC.begin(),
            record_page_layout::MAGIC.end(),
            bytes_.begin() + record_page_layout::MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid record page magic/type.");
    }

    const auto version =
        readUint32LittleEndian(bytes_, record_page_layout::LAYOUT_VERSION_OFFSET);
    if (version != record_page_layout::CURRENT_VERSION) {
        throw std::runtime_error(
            "Unsupported record page layout version " + std::to_string(version) + ".");
    }

    const auto headerSize =
        readUint32LittleEndian(bytes_, record_page_layout::HEADER_SIZE_OFFSET);
    if (headerSize != record_page_layout::HEADER_SIZE) {
        throw std::runtime_error("Record page has an invalid header size.");
    }

    const auto storedCapacity =
        readUint16LittleEndian(bytes_, record_page_layout::SLOT_CAPACITY_OFFSET);
    if (storedCapacity != record_page_layout::SLOT_CAPACITY) {
        throw std::runtime_error("Record page has an incompatible slot capacity.");
    }

    const auto storedSlotSize =
        readUint32LittleEndian(bytes_, record_page_layout::SLOT_SIZE_OFFSET);
    if (storedSlotSize != row_layout::SERIALIZED_SIZE) {
        throw std::runtime_error("Record page has an incompatible slot size.");
    }

    const auto reserved =
        readUint32LittleEndian(bytes_, record_page_layout::RESERVED_OFFSET);
    if (reserved != 0) {
        throw std::runtime_error("Record page reserved header field is not zero.");
    }

    validateNextPageId(nextPageId());

    std::size_t occupiedCount = 0;
    const auto bitmapBitCount = record_page_layout::OCCUPANCY_SIZE * BITS_PER_BYTE;
    for (std::size_t index = 0; index < bitmapBitCount; ++index) {
        if (!isOccupiedUnchecked(index)) {
            continue;
        }
        if (index >= record_page_layout::SLOT_CAPACITY) {
            throw std::runtime_error("Record page has occupied bits outside its slot capacity.");
        }
        ++occupiedCount;
    }

    if (liveCount() > record_page_layout::SLOT_CAPACITY) {
        throw std::runtime_error("Record page live count exceeds slot capacity.");
    }
    if (occupiedCount != liveCount()) {
        throw std::runtime_error("Record page live count does not match occupancy metadata.");
    }
}

void RecordPage::validateNextPageId(PageId nextPageId) const {
    if (nextPageId == INVALID_PAGE_ID) {
        return;
    }
    if (nextPageId == database_format::METADATA_PAGE_ID
        || nextPageId == pageId_
        || nextPageId >= pager_.pageCount()) {
        throw std::runtime_error("Record page contains an invalid next-page ID.");
    }
}

void RecordPage::validateSlotId(SlotId slotId) {
    if (slotId >= record_page_layout::SLOT_CAPACITY) {
        throw std::out_of_range("Record slot ID is outside the page capacity.");
    }
}

bool RecordPage::isOccupiedUnchecked(std::size_t slotIndex) const noexcept {
    const auto byteIndex = slotIndex / BITS_PER_BYTE;
    const auto bitIndex = slotIndex % BITS_PER_BYTE;
    const auto mask = static_cast<std::uint8_t>(1U << bitIndex);
    const auto value = std::to_integer<std::uint8_t>(
        bytes_[record_page_layout::OCCUPANCY_OFFSET + byteIndex]);
    return (value & mask) != 0;
}

void RecordPage::setOccupied(SlotId slotId, bool occupied) noexcept {
    const auto byteIndex = static_cast<std::size_t>(slotId) / BITS_PER_BYTE;
    const auto bitIndex = static_cast<std::size_t>(slotId) % BITS_PER_BYTE;
    const auto mask = static_cast<std::uint8_t>(1U << bitIndex);
    auto value = std::to_integer<std::uint8_t>(
        bytes_[record_page_layout::OCCUPANCY_OFFSET + byteIndex]);
    value = occupied ? static_cast<std::uint8_t>(value | mask)
                     : static_cast<std::uint8_t>(value & static_cast<std::uint8_t>(~mask));
    bytes_[record_page_layout::OCCUPANCY_OFFSET + byteIndex] =
        static_cast<std::byte>(value);
}

} // namespace minidb
