#include "minidb/slotted_page.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/storage_error.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace minidb {
namespace {

void requireExistingDataPage(
    PageId pageCount,
    PageId pageId,
    PageId excludedPageId,
    const char* description) {
    if (pageId == database_format::METADATA_PAGE_ID || pageId == INVALID_PAGE_ID
        || pageId == excludedPageId || pageId >= pageCount) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            std::string(description) + " is not a legal data-page reference.");
    }
}

} // namespace

void SlottedPageView::initialize(
    std::span<std::byte, database_format::PAGE_SIZE> bytes,
    PageId pageId,
    PageId pageCount,
    PageId heapMetadataPageId,
    PageId nextPageId,
    PageId previousPageId) {
    requireExistingDataPage(pageCount, pageId, heapMetadataPageId, "Slotted page ID");
    requireExistingDataPage(pageCount, heapMetadataPageId, pageId, "Heap metadata page ID");
    const auto validateLink = [&](PageId link, const char* description) {
        if (link != INVALID_PAGE_ID) {
            requireExistingDataPage(pageCount, link, pageId, description);
            if (link == heapMetadataPageId) {
                throw StorageError(
                    StorageErrorKind::InvalidPage,
                    std::string(description) + " references heap metadata.");
            }
        }
    };
    validateLink(nextPageId, "Next slotted-page ID");
    validateLink(previousPageId, "Previous slotted-page ID");
    if (nextPageId != INVALID_PAGE_ID && nextPageId == previousPageId) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            "Slotted page next and previous links are identical.");
    }

    using namespace slotted_page_layout;
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(bytes, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(bytes, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    byte_codec::writeUint16(bytes, SLOT_COUNT_OFFSET, 0);
    byte_codec::writeUint16(bytes, LIVE_COUNT_OFFSET, 0);
    byte_codec::writeUint16(bytes, LOWER_BOUNDARY_OFFSET, static_cast<std::uint16_t>(HEADER_SIZE));
    byte_codec::writeUint16(
        bytes, UPPER_BOUNDARY_OFFSET, static_cast<std::uint16_t>(database_format::PAGE_SIZE));
    byte_codec::writeUint32(bytes, NEXT_PAGE_ID_OFFSET, nextPageId);
    byte_codec::writeUint32(bytes, PREVIOUS_PAGE_ID_OFFSET, previousPageId);
    byte_codec::writeUint32(bytes, HEAP_METADATA_PAGE_ID_OFFSET, heapMetadataPageId);
}

ConstSlottedPageView::ConstSlottedPageView(
    std::span<const std::byte, database_format::PAGE_SIZE> bytes,
    PageId pageId,
    PageId pageCount)
    : bytes_(bytes), pageId_(pageId), pageCount_(pageCount) {
    validate();
}

SlottedPageView::SlottedPageView(
    std::span<std::byte, database_format::PAGE_SIZE> bytes,
    PageId pageId,
    PageId pageCount)
    : ConstSlottedPageView(bytes, pageId, pageCount), mutableBytes_(bytes) {}

PageId ConstSlottedPageView::heapMetadataPageId() const noexcept {
    return byte_codec::readUint32(bytes_, slotted_page_layout::HEAP_METADATA_PAGE_ID_OFFSET);
}

PageId ConstSlottedPageView::nextPageId() const noexcept {
    return byte_codec::readUint32(bytes_, slotted_page_layout::NEXT_PAGE_ID_OFFSET);
}

PageId ConstSlottedPageView::previousPageId() const noexcept {
    return byte_codec::readUint32(bytes_, slotted_page_layout::PREVIOUS_PAGE_ID_OFFSET);
}

void SlottedPageView::setNextPageId(PageId pageId) {
    validateLink(pageId);
    if (pageId != INVALID_PAGE_ID && pageId == previousPageId()) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            "Slotted page next and previous links cannot match.");
    }
    byte_codec::writeUint32(mutableBytes_, slotted_page_layout::NEXT_PAGE_ID_OFFSET, pageId);
}

void SlottedPageView::setPreviousPageId(PageId pageId) {
    validateLink(pageId);
    if (pageId != INVALID_PAGE_ID && pageId == nextPageId()) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            "Slotted page next and previous links cannot match.");
    }
    byte_codec::writeUint32(mutableBytes_, slotted_page_layout::PREVIOUS_PAGE_ID_OFFSET, pageId);
}

std::uint16_t ConstSlottedPageView::slotCount() const noexcept {
    return byte_codec::readUint16(bytes_, slotted_page_layout::SLOT_COUNT_OFFSET);
}

std::uint16_t ConstSlottedPageView::liveCount() const noexcept {
    return byte_codec::readUint16(bytes_, slotted_page_layout::LIVE_COUNT_OFFSET);
}

std::uint16_t ConstSlottedPageView::lowerBoundary() const noexcept {
    return byte_codec::readUint16(bytes_, slotted_page_layout::LOWER_BOUNDARY_OFFSET);
}

std::uint16_t ConstSlottedPageView::upperBoundary() const noexcept {
    return byte_codec::readUint16(bytes_, slotted_page_layout::UPPER_BOUNDARY_OFFSET);
}

std::size_t ConstSlottedPageView::freeSpace() const noexcept {
    return static_cast<std::size_t>(upperBoundary() - lowerBoundary());
}

bool ConstSlottedPageView::canFit(std::size_t tupleSize) const noexcept {
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

bool ConstSlottedPageView::isOccupied(SlotId slotId) const {
    validateSlotId(slotId);
    return readSlot(slotId).flags == slotted_page_layout::SLOT_LIVE;
}

SlotId SlottedPageView::insert(std::span<const std::byte> tuple) {
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
        byte_codec::writeUint16(
            mutableBytes_, slotted_page_layout::SLOT_COUNT_OFFSET, slotCount() + 1);
        byte_codec::writeUint16(
            mutableBytes_,
            slotted_page_layout::UPPER_BOUNDARY_OFFSET,
            static_cast<std::uint16_t>(upperBoundary() - slotted_page_layout::SLOT_SIZE));
    }

    const auto tupleOffset = lowerBoundary();
    std::copy(tuple.begin(), tuple.end(), mutableBytes_.begin() + tupleOffset);
    writeSlot(slotId, SlotEntry{
        tupleOffset,
        static_cast<std::uint16_t>(tuple.size()),
        slotted_page_layout::SLOT_LIVE,
        0,
    });
    byte_codec::writeUint16(
        mutableBytes_,
        slotted_page_layout::LOWER_BOUNDARY_OFFSET,
        static_cast<std::uint16_t>(tupleOffset + tuple.size()));
    byte_codec::writeUint16(
        mutableBytes_, slotted_page_layout::LIVE_COUNT_OFFSET, liveCount() + 1);
    return slotId;
}

TupleBytes ConstSlottedPageView::get(SlotId slotId) const {
    validateSlotId(slotId);
    const auto slot = readSlot(slotId);
    if (slot.flags != slotted_page_layout::SLOT_LIVE) {
        throw std::runtime_error("Slotted-page slot is not occupied.");
    }
    return TupleBytes(
        bytes_.begin() + slot.tupleOffset,
        bytes_.begin() + slot.tupleOffset + slot.tupleLength);
}

bool SlottedPageView::tryUpdate(SlotId slotId, std::span<const std::byte> tuple) {
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
        std::copy(tuple.begin(), tuple.end(), mutableBytes_.begin() + slot.tupleOffset);
        return true;
    }

    auto tuples = liveTuples();
    for (auto& [candidate, bytes] : tuples) {
        if (candidate == slotId) {
            bytes.assign(tuple.begin(), tuple.end());
            break;
        }
    }
    rewritePayload(tuples);
    return true;
}

void SlottedPageView::erase(SlotId slotId) {
    validateSlotId(slotId);
    const auto slot = readSlot(slotId);
    if (slot.flags != slotted_page_layout::SLOT_LIVE) {
        throw std::runtime_error("Slotted-page slot is not occupied.");
    }
    writeSlot(slotId, SlotEntry{0, 0, slotted_page_layout::SLOT_FREE, 0});
    byte_codec::writeUint16(
        mutableBytes_, slotted_page_layout::LIVE_COUNT_OFFSET, liveCount() - 1);
    compact();
}

void SlottedPageView::compact() {
    rewritePayload(liveTuples());
}

ConstSlottedPageView::SlotEntry ConstSlottedPageView::readSlot(SlotId slotId) const noexcept {
    const auto offset = slotted_page_layout::slotOffset(slotId);
    return SlotEntry{
        byte_codec::readUint16(bytes_, offset + slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET),
        byte_codec::readUint16(bytes_, offset + slotted_page_layout::SLOT_TUPLE_LENGTH_OFFSET),
        byte_codec::readUint16(bytes_, offset + slotted_page_layout::SLOT_FLAGS_OFFSET),
        byte_codec::readUint16(bytes_, offset + slotted_page_layout::SLOT_RESERVED_OFFSET),
    };
}

void SlottedPageView::writeSlot(SlotId slotId, const SlotEntry& slot) noexcept {
    const auto offset = slotted_page_layout::slotOffset(slotId);
    byte_codec::writeUint16(
        mutableBytes_, offset + slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET, slot.tupleOffset);
    byte_codec::writeUint16(
        mutableBytes_, offset + slotted_page_layout::SLOT_TUPLE_LENGTH_OFFSET, slot.tupleLength);
    byte_codec::writeUint16(
        mutableBytes_, offset + slotted_page_layout::SLOT_FLAGS_OFFSET, slot.flags);
    byte_codec::writeUint16(
        mutableBytes_, offset + slotted_page_layout::SLOT_RESERVED_OFFSET, slot.reserved);
}

void ConstSlottedPageView::validateSlotId(SlotId slotId) const {
    if (slotId >= slotCount()) {
        throw std::out_of_range("Slot ID is outside the slotted-page directory.");
    }
}

void ConstSlottedPageView::validateLink(PageId linkedPageId) const {
    if (linkedPageId == INVALID_PAGE_ID) {
        return;
    }
    requireExistingDataPage(pageCount_, linkedPageId, pageId_, "Slotted-page link");
    if (linkedPageId == heapMetadataPageId()) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            "Slotted-page link references heap metadata.");
    }
}

void ConstSlottedPageView::validateTupleSize(std::size_t tupleSize) {
    if (tupleSize == 0) {
        throw std::invalid_argument("Zero-length tuples are not supported.");
    }
    if (tupleSize > slotted_page_layout::MAX_TUPLE_SIZE
        || tupleSize > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("Tuple exceeds the maximum inline slotted-page size.");
    }
}

std::vector<std::pair<SlotId, TupleBytes>> ConstSlottedPageView::liveTuples() const {
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

void SlottedPageView::rewritePayload(
    const std::vector<std::pair<SlotId, TupleBytes>>& tuples) {
    std::fill(
        mutableBytes_.begin() + slotted_page_layout::HEADER_SIZE,
        mutableBytes_.begin() + upperBoundary(),
        std::byte{0});
    std::size_t cursor = slotted_page_layout::HEADER_SIZE;
    for (const auto& [slotId, tuple] : tuples) {
        std::copy(tuple.begin(), tuple.end(), mutableBytes_.begin() + cursor);
        writeSlot(slotId, SlotEntry{
            static_cast<std::uint16_t>(cursor),
            static_cast<std::uint16_t>(tuple.size()),
            slotted_page_layout::SLOT_LIVE,
            0,
        });
        cursor += tuple.size();
    }
    byte_codec::writeUint16(
        mutableBytes_,
        slotted_page_layout::LOWER_BOUNDARY_OFFSET,
        static_cast<std::uint16_t>(cursor));
}

void ConstSlottedPageView::validate() const {
    using namespace slotted_page_layout;
    if (!std::equal(MAGIC.begin(), MAGIC.end(), bytes_.begin() + MAGIC_OFFSET)) {
        throw StorageError(StorageErrorKind::CorruptPage, "Invalid slotted-page magic/type.");
    }
    const auto version = byte_codec::readUint32(bytes_, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Unsupported slotted-page layout version " + std::to_string(version) + ".");
    }
    if (byte_codec::readUint32(bytes_, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted page has an invalid header size.");
    }
    if (!std::all_of(
            bytes_.begin() + RESERVED_OFFSET,
            bytes_.begin() + HEADER_SIZE,
            [](std::byte value) { return value == std::byte{0}; })) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted-page reserved header bytes are not zero.");
    }

    requireExistingDataPage(pageCount_, heapMetadataPageId(), pageId_, "Heap metadata page ID");
    validateLink(nextPageId());
    validateLink(previousPageId());
    if (nextPageId() != INVALID_PAGE_ID && nextPageId() == previousPageId()) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted page has identical next and previous links.");
    }

    if (slotCount() > MAX_SLOT_COUNT || liveCount() > slotCount()) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted page has invalid slot/live counts.");
    }
    const auto expectedUpper = database_format::PAGE_SIZE
        - (static_cast<std::size_t>(slotCount()) * SLOT_SIZE);
    if (upperBoundary() != expectedUpper
        || lowerBoundary() < HEADER_SIZE
        || lowerBoundary() > upperBoundary()) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted page has invalid free-space boundaries.");
    }

    std::size_t observedLiveCount = 0;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(liveCount());
    for (std::size_t index = 0; index < slotCount(); ++index) {
        const auto slot = readSlot(static_cast<SlotId>(index));
        if (slot.flags == SLOT_FREE) {
            if (slot.tupleOffset != 0 || slot.tupleLength != 0 || slot.reserved != 0) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Slotted page contains a malformed free slot.");
            }
            continue;
        }
        if (slot.flags != SLOT_LIVE || slot.reserved != 0 || slot.tupleLength == 0) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Slotted page contains an invalid live slot.");
        }
        const auto begin = static_cast<std::size_t>(slot.tupleOffset);
        const auto end = begin + slot.tupleLength;
        if (begin < HEADER_SIZE || end > lowerBoundary() || end > upperBoundary()) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Slotted-page tuple range is outside the payload area.");
        }
        ranges.emplace_back(begin, end);
        ++observedLiveCount;
    }
    if (observedLiveCount != liveCount()) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted-page live count disagrees with slot states.");
    }
    std::sort(ranges.begin(), ranges.end());
    std::size_t cursor = HEADER_SIZE;
    for (const auto& [begin, end] : ranges) {
        if (begin != cursor) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Slotted-page tuple payloads overlap or are not compact.");
        }
        cursor = end;
    }
    if (cursor != lowerBoundary()) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted-page lower boundary disagrees with tuple payloads.");
    }
    if (!std::all_of(
            bytes_.begin() + lowerBoundary(),
            bytes_.begin() + upperBoundary(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Slotted-page contiguous free space is not zero-filled.");
    }
}

} // namespace minidb
