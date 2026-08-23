#include "minidb/slotted_page.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace minidb {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

void writeUint16(Pager::Page& page, std::size_t offset, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < 2; ++index) {
        page[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

void writeUint32(Pager::Page& page, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

std::uint16_t readUint16(const Pager::Page& page, std::size_t offset) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(page[offset + index])
            << (index * BITS_PER_BYTE));
    }
    return value;
}

std::uint32_t readUint32(const Pager::Page& page, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index])
            << (index * BITS_PER_BYTE);
    }
    return value;
}

void requireExistingDataPage(
    const Pager& pager,
    PageId pageId,
    PageId excludedPageId,
    const char* description) {
    if (pageId == database_format::METADATA_PAGE_ID || pageId == INVALID_PAGE_ID
        || pageId == excludedPageId || pageId >= pager.pageCount()) {
        throw std::runtime_error(std::string(description) + " is not a legal data-page reference.");
    }
}

} // namespace

void SlottedPage::initialize(
    Pager& pager,
    PageId pageId,
    PageId heapMetadataPageId,
    PageId nextPageId,
    PageId previousPageId) {
    requireExistingDataPage(pager, pageId, heapMetadataPageId, "Slotted page ID");
    requireExistingDataPage(pager, heapMetadataPageId, pageId, "Heap metadata page ID");
    const auto validateLink = [&](PageId link, const char* description) {
        if (link != INVALID_PAGE_ID) {
            requireExistingDataPage(pager, link, pageId, description);
            if (link == heapMetadataPageId) {
                throw std::runtime_error(std::string(description) + " references heap metadata.");
            }
        }
    };
    validateLink(nextPageId, "Next slotted-page ID");
    validateLink(previousPageId, "Previous slotted-page ID");
    if (nextPageId != INVALID_PAGE_ID && nextPageId == previousPageId) {
        throw std::runtime_error("Slotted page next and previous links are identical.");
    }

    auto& page = pager.getPage(pageId);
    page.fill(std::byte{0});
    using namespace slotted_page_layout;
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    writeUint16(page, SLOT_COUNT_OFFSET, 0);
    writeUint16(page, LIVE_COUNT_OFFSET, 0);
    writeUint16(page, LOWER_BOUNDARY_OFFSET, static_cast<std::uint16_t>(HEADER_SIZE));
    writeUint16(page, UPPER_BOUNDARY_OFFSET, static_cast<std::uint16_t>(Pager::PAGE_SIZE));
    writeUint32(page, NEXT_PAGE_ID_OFFSET, nextPageId);
    writeUint32(page, PREVIOUS_PAGE_ID_OFFSET, previousPageId);
    writeUint32(page, HEAP_METADATA_PAGE_ID_OFFSET, heapMetadataPageId);
    pager.markDirty(pageId);
}

SlottedPage::SlottedPage(Pager& pager, PageId pageId)
    : pager_(pager), pageId_(pageId), bytes_(pager.getPage(pageId)) {
    validate();
}

PageId SlottedPage::heapMetadataPageId() const noexcept {
    return readUint32(bytes_, slotted_page_layout::HEAP_METADATA_PAGE_ID_OFFSET);
}

PageId SlottedPage::nextPageId() const noexcept {
    return readUint32(bytes_, slotted_page_layout::NEXT_PAGE_ID_OFFSET);
}

PageId SlottedPage::previousPageId() const noexcept {
    return readUint32(bytes_, slotted_page_layout::PREVIOUS_PAGE_ID_OFFSET);
}

void SlottedPage::setNextPageId(PageId pageId) {
    validateLink(pageId);
    if (pageId != INVALID_PAGE_ID && pageId == previousPageId()) {
        throw std::runtime_error("Slotted page next and previous links cannot match.");
    }
    writeUint32(bytes_, slotted_page_layout::NEXT_PAGE_ID_OFFSET, pageId);
    pager_.markDirty(pageId_);
}

void SlottedPage::setPreviousPageId(PageId pageId) {
    validateLink(pageId);
    if (pageId != INVALID_PAGE_ID && pageId == nextPageId()) {
        throw std::runtime_error("Slotted page next and previous links cannot match.");
    }
    writeUint32(bytes_, slotted_page_layout::PREVIOUS_PAGE_ID_OFFSET, pageId);
    pager_.markDirty(pageId_);
}

std::uint16_t SlottedPage::slotCount() const noexcept {
    return readUint16(bytes_, slotted_page_layout::SLOT_COUNT_OFFSET);
}

std::uint16_t SlottedPage::liveCount() const noexcept {
    return readUint16(bytes_, slotted_page_layout::LIVE_COUNT_OFFSET);
}

std::uint16_t SlottedPage::lowerBoundary() const noexcept {
    return readUint16(bytes_, slotted_page_layout::LOWER_BOUNDARY_OFFSET);
}

std::uint16_t SlottedPage::upperBoundary() const noexcept {
    return readUint16(bytes_, slotted_page_layout::UPPER_BOUNDARY_OFFSET);
}

std::size_t SlottedPage::freeSpace() const noexcept {
    return static_cast<std::size_t>(upperBoundary() - lowerBoundary());
}

bool SlottedPage::canFit(std::size_t tupleSize) const noexcept {
    if (tupleSize == 0 || tupleSize > slotted_page_layout::MAX_TUPLE_SIZE) {
        return false;
    }
    bool reusableSlot = false;
    for (std::size_t index = 0; index < slotCount(); ++index) {
        if (readSlot(static_cast<SlotId>(index)).flags == slotted_page_layout::SLOT_FREE) {
            reusableSlot = true;
            break;
        }
    }
    const auto required = tupleSize + (reusableSlot ? 0 : slotted_page_layout::SLOT_SIZE);
    return required <= freeSpace();
}

bool SlottedPage::isOccupied(SlotId slotId) const {
    validateSlotId(slotId);
    return readSlot(slotId).flags == slotted_page_layout::SLOT_LIVE;
}

SlotId SlottedPage::insert(std::span<const std::byte> tuple) {
    validateTupleSize(tuple.size());
    if (!canFit(tuple.size())) {
        throw std::overflow_error("Slotted page has insufficient space for tuple and slot.");
    }

    SlotId slotId = INVALID_SLOT_ID;
    for (std::size_t index = 0; index < slotCount(); ++index) {
        const auto candidate = static_cast<SlotId>(index);
        if (readSlot(candidate).flags == slotted_page_layout::SLOT_FREE) {
            slotId = candidate;
            break;
        }
    }
    if (slotId == INVALID_SLOT_ID) {
        if (slotCount() >= slotted_page_layout::MAX_SLOT_COUNT) {
            throw std::overflow_error("Slotted page slot directory is full.");
        }
        slotId = slotCount();
        writeUint16(bytes_, slotted_page_layout::SLOT_COUNT_OFFSET, slotCount() + 1);
        writeUint16(
            bytes_,
            slotted_page_layout::UPPER_BOUNDARY_OFFSET,
            static_cast<std::uint16_t>(upperBoundary() - slotted_page_layout::SLOT_SIZE));
    }

    const auto tupleOffset = lowerBoundary();
    std::copy(tuple.begin(), tuple.end(), bytes_.begin() + tupleOffset);
    writeSlot(slotId, SlotEntry{
        tupleOffset,
        static_cast<std::uint16_t>(tuple.size()),
        slotted_page_layout::SLOT_LIVE,
        0,
    });
    writeUint16(
        bytes_,
        slotted_page_layout::LOWER_BOUNDARY_OFFSET,
        static_cast<std::uint16_t>(tupleOffset + tuple.size()));
    writeUint16(bytes_, slotted_page_layout::LIVE_COUNT_OFFSET, liveCount() + 1);
    pager_.markDirty(pageId_);
    return slotId;
}

TupleBytes SlottedPage::get(SlotId slotId) const {
    validateSlotId(slotId);
    const auto slot = readSlot(slotId);
    if (slot.flags != slotted_page_layout::SLOT_LIVE) {
        throw std::runtime_error("Slotted-page slot is not occupied.");
    }
    return TupleBytes(
        bytes_.begin() + slot.tupleOffset,
        bytes_.begin() + slot.tupleOffset + slot.tupleLength);
}

bool SlottedPage::tryUpdate(SlotId slotId, std::span<const std::byte> tuple) {
    validateTupleSize(tuple.size());
    validateSlotId(slotId);
    const auto slot = readSlot(slotId);
    if (slot.flags != slotted_page_layout::SLOT_LIVE) {
        throw std::runtime_error("Slotted-page slot is not occupied.");
    }
    if (tuple.size() > freeSpace() + slot.tupleLength) {
        return false;
    }
    if (tuple.size() == slot.tupleLength) {
        std::copy(tuple.begin(), tuple.end(), bytes_.begin() + slot.tupleOffset);
        pager_.markDirty(pageId_);
        return true;
    }

    auto tuples = snapshotLiveTuples();
    for (auto& [candidate, bytes] : tuples) {
        if (candidate == slotId) {
            bytes.assign(tuple.begin(), tuple.end());
            break;
        }
    }
    rewritePayload(tuples);
    return true;
}

void SlottedPage::erase(SlotId slotId) {
    validateSlotId(slotId);
    const auto slot = readSlot(slotId);
    if (slot.flags != slotted_page_layout::SLOT_LIVE) {
        throw std::runtime_error("Slotted-page slot is not occupied.");
    }
    writeSlot(slotId, SlotEntry{0, 0, slotted_page_layout::SLOT_FREE, 0});
    writeUint16(bytes_, slotted_page_layout::LIVE_COUNT_OFFSET, liveCount() - 1);
    compact();
}

void SlottedPage::compact() {
    rewritePayload(snapshotLiveTuples());
}

SlottedPage::SlotEntry SlottedPage::readSlot(SlotId slotId) const noexcept {
    const auto offset = slotted_page_layout::slotOffset(slotId);
    return SlotEntry{
        readUint16(bytes_, offset + slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET),
        readUint16(bytes_, offset + slotted_page_layout::SLOT_TUPLE_LENGTH_OFFSET),
        readUint16(bytes_, offset + slotted_page_layout::SLOT_FLAGS_OFFSET),
        readUint16(bytes_, offset + slotted_page_layout::SLOT_RESERVED_OFFSET),
    };
}

void SlottedPage::writeSlot(SlotId slotId, const SlotEntry& slot) noexcept {
    const auto offset = slotted_page_layout::slotOffset(slotId);
    writeUint16(bytes_, offset + slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET, slot.tupleOffset);
    writeUint16(bytes_, offset + slotted_page_layout::SLOT_TUPLE_LENGTH_OFFSET, slot.tupleLength);
    writeUint16(bytes_, offset + slotted_page_layout::SLOT_FLAGS_OFFSET, slot.flags);
    writeUint16(bytes_, offset + slotted_page_layout::SLOT_RESERVED_OFFSET, slot.reserved);
}

void SlottedPage::validateSlotId(SlotId slotId) const {
    if (slotId >= slotCount()) {
        throw std::out_of_range("Slot ID is outside the slotted-page directory.");
    }
}

void SlottedPage::validateLink(PageId linkedPageId) const {
    if (linkedPageId == INVALID_PAGE_ID) {
        return;
    }
    requireExistingDataPage(pager_, linkedPageId, pageId_, "Slotted-page link");
    if (linkedPageId == heapMetadataPageId()) {
        throw std::runtime_error("Slotted-page link references heap metadata.");
    }
}

void SlottedPage::validateTupleSize(std::size_t tupleSize) {
    if (tupleSize == 0) {
        throw std::invalid_argument("Zero-length tuples are not supported.");
    }
    if (tupleSize > slotted_page_layout::MAX_TUPLE_SIZE
        || tupleSize > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("Tuple exceeds the maximum inline slotted-page size.");
    }
}

std::vector<std::pair<SlotId, TupleBytes>> SlottedPage::snapshotLiveTuples() const {
    std::vector<std::pair<SlotId, TupleBytes>> tuples;
    tuples.reserve(liveCount());
    for (std::size_t index = 0; index < slotCount(); ++index) {
        const auto slotId = static_cast<SlotId>(index);
        if (readSlot(slotId).flags == slotted_page_layout::SLOT_LIVE) {
            tuples.emplace_back(slotId, get(slotId));
        }
    }
    return tuples;
}

void SlottedPage::rewritePayload(
    const std::vector<std::pair<SlotId, TupleBytes>>& tuples) {
    std::fill(
        bytes_.begin() + slotted_page_layout::HEADER_SIZE,
        bytes_.begin() + upperBoundary(),
        std::byte{0});
    std::size_t cursor = slotted_page_layout::HEADER_SIZE;
    for (const auto& [slotId, tuple] : tuples) {
        std::copy(tuple.begin(), tuple.end(), bytes_.begin() + cursor);
        writeSlot(slotId, SlotEntry{
            static_cast<std::uint16_t>(cursor),
            static_cast<std::uint16_t>(tuple.size()),
            slotted_page_layout::SLOT_LIVE,
            0,
        });
        cursor += tuple.size();
    }
    writeUint16(
        bytes_,
        slotted_page_layout::LOWER_BOUNDARY_OFFSET,
        static_cast<std::uint16_t>(cursor));
    pager_.markDirty(pageId_);
}

void SlottedPage::validate() const {
    using namespace slotted_page_layout;
    if (!std::equal(MAGIC.begin(), MAGIC.end(), bytes_.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid slotted-page magic/type.");
    }
    const auto version = readUint32(bytes_, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw std::runtime_error("Unsupported slotted-page layout version "
                                 + std::to_string(version) + ".");
    }
    if (readUint32(bytes_, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw std::runtime_error("Slotted page has an invalid header size.");
    }
    if (!std::all_of(
            bytes_.begin() + RESERVED_OFFSET,
            bytes_.begin() + HEADER_SIZE,
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Slotted-page reserved header bytes are not zero.");
    }

    requireExistingDataPage(pager_, heapMetadataPageId(), pageId_, "Heap metadata page ID");
    validateLink(nextPageId());
    validateLink(previousPageId());
    if (nextPageId() != INVALID_PAGE_ID && nextPageId() == previousPageId()) {
        throw std::runtime_error("Slotted page has identical next and previous links.");
    }

    if (slotCount() > MAX_SLOT_COUNT || liveCount() > slotCount()) {
        throw std::runtime_error("Slotted page has invalid slot/live counts.");
    }
    const auto expectedUpper = Pager::PAGE_SIZE
        - (static_cast<std::size_t>(slotCount()) * SLOT_SIZE);
    if (upperBoundary() != expectedUpper
        || lowerBoundary() < HEADER_SIZE
        || lowerBoundary() > upperBoundary()) {
        throw std::runtime_error("Slotted page has invalid free-space boundaries.");
    }

    std::size_t observedLiveCount = 0;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(liveCount());
    for (std::size_t index = 0; index < slotCount(); ++index) {
        const auto slot = readSlot(static_cast<SlotId>(index));
        if (slot.flags == SLOT_FREE) {
            if (slot.tupleOffset != 0 || slot.tupleLength != 0 || slot.reserved != 0) {
                throw std::runtime_error("Slotted page contains a malformed free slot.");
            }
            continue;
        }
        if (slot.flags != SLOT_LIVE || slot.reserved != 0 || slot.tupleLength == 0) {
            throw std::runtime_error("Slotted page contains an invalid live slot.");
        }
        const auto begin = static_cast<std::size_t>(slot.tupleOffset);
        const auto end = begin + slot.tupleLength;
        if (begin < HEADER_SIZE || end > lowerBoundary() || end > upperBoundary()) {
            throw std::runtime_error("Slotted-page tuple range is outside the payload area.");
        }
        ranges.emplace_back(begin, end);
        ++observedLiveCount;
    }
    if (observedLiveCount != liveCount()) {
        throw std::runtime_error("Slotted-page live count disagrees with slot states.");
    }
    std::sort(ranges.begin(), ranges.end());
    std::size_t cursor = HEADER_SIZE;
    for (const auto& [begin, end] : ranges) {
        if (begin != cursor) {
            throw std::runtime_error("Slotted-page tuple payloads overlap or are not compact.");
        }
        cursor = end;
    }
    if (cursor != lowerBoundary()) {
        throw std::runtime_error("Slotted-page lower boundary disagrees with tuple payloads.");
    }
    if (!std::all_of(
            bytes_.begin() + lowerBoundary(),
            bytes_.begin() + upperBoundary(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Slotted-page contiguous free space is not zero-filled.");
    }
}

} // namespace minidb
