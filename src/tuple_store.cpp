#include "minidb/tuple_store.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace minidb {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

void writeUint32(Pager::Page& page, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

void writeUint64(Pager::Page& page, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        page[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

std::uint32_t readUint32(const Pager::Page& page, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index])
            << (index * BITS_PER_BYTE);
    }
    return value;
}

std::uint64_t readUint64(const Pager::Page& page, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(page[offset + index])
            << (index * BITS_PER_BYTE);
    }
    return value;
}

} // namespace

TupleStore TupleStore::create(Pager& pager) {
    PageAllocator allocator(pager);
    const auto metadataPageId = allocator.allocatePage();
    TupleStore store(pager, metadataPageId);
    store.writeMetadata(Metadata{});
    return store;
}

TupleStore TupleStore::open(Pager& pager, PageId metadataPageId) {
    TupleStore store(pager, metadataPageId);
    store.validate();
    return store;
}

PageId TupleStore::firstPageId() const {
    return readMetadata().firstPageId;
}

PageId TupleStore::lastPageId() const {
    return readMetadata().lastPageId;
}

std::uint64_t TupleStore::size() const {
    return readMetadata().tupleCount;
}

bool TupleStore::empty() const {
    return size() == 0;
}

void TupleStore::validateMetadataPageId() const {
    if (metadataPageId_ == database_format::METADATA_PAGE_ID
        || metadataPageId_ == INVALID_PAGE_ID
        || metadataPageId_ >= pager_.pageCount()) {
        throw std::out_of_range("Tuple heap metadata page ID does not exist.");
    }
}

TupleStore::Metadata TupleStore::readMetadata() const {
    validateMetadataPageId();
    const auto& page = pager_.getPage(metadataPageId_);
    using namespace tuple_heap_metadata_layout;
    if (!std::equal(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid tuple heap metadata magic/type.");
    }
    const auto version = readUint32(page, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw std::runtime_error(
            "Unsupported tuple heap metadata version " + std::to_string(version) + ".");
    }
    if (readUint32(page, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw std::runtime_error("Tuple heap metadata has an invalid header size.");
    }
    if (!std::all_of(
            page.begin() + RESERVED_OFFSET,
            page.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Tuple heap metadata reserved bytes are not zero.");
    }

    Metadata metadata{
        readUint32(page, FIRST_PAGE_ID_OFFSET),
        readUint32(page, LAST_PAGE_ID_OFFSET),
        readUint64(page, TUPLE_COUNT_OFFSET),
    };
    const auto validPageReference = [&](PageId pageId) {
        return pageId != database_format::METADATA_PAGE_ID
            && pageId != INVALID_PAGE_ID
            && pageId != metadataPageId_
            && pageId < pager_.pageCount();
    };
    if (metadata.tupleCount == 0) {
        if (metadata.firstPageId != INVALID_PAGE_ID
            || metadata.lastPageId != INVALID_PAGE_ID) {
            throw std::runtime_error("Empty tuple heap metadata retains data-page links.");
        }
    } else if (!validPageReference(metadata.firstPageId)
               || !validPageReference(metadata.lastPageId)) {
        throw std::runtime_error("Nonempty tuple heap metadata has invalid first/last pages.");
    }
    if ((metadata.firstPageId == INVALID_PAGE_ID)
        != (metadata.lastPageId == INVALID_PAGE_ID)) {
        throw std::runtime_error("Tuple heap metadata first/last page state disagrees.");
    }
    return metadata;
}

void TupleStore::writeMetadata(const Metadata& metadata) {
    validateMetadataPageId();
    auto& page = pager_.getPage(metadataPageId_);
    using namespace tuple_heap_metadata_layout;
    page.fill(std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    writeUint32(page, FIRST_PAGE_ID_OFFSET, metadata.firstPageId);
    writeUint32(page, LAST_PAGE_ID_OFFSET, metadata.lastPageId);
    writeUint64(page, TUPLE_COUNT_OFFSET, metadata.tupleCount);
    pager_.markDirty(metadataPageId_);
}

void TupleStore::validateTupleSize(std::size_t size) {
    if (size == 0) {
        throw std::invalid_argument("Zero-length tuples are not supported.");
    }
    if (size > slotted_page_layout::MAX_TUPLE_SIZE) {
        throw std::invalid_argument("Tuple exceeds the maximum inline slotted-page size.");
    }
}

SlottedPage TupleStore::openOwnedPage(PageId pageId) const {
    SlottedPage page(pager_, pageId);
    if (page.heapMetadataPageId() != metadataPageId_) {
        throw std::out_of_range("Slotted page belongs to a different tuple heap.");
    }
    return page;
}

SlottedPage TupleStore::openRecordPage(RecordId recordId) const {
    if (!recordId.isValid() || recordId.pageId >= pager_.pageCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple-page ID.");
    }
    auto page = openOwnedPage(recordId.pageId);
    if (recordId.slotId >= page.slotCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple SlotId.");
    }
    return page;
}

RecordId TupleStore::insert(std::span<const std::byte> tuple) {
    validateTupleSize(tuple.size());
    auto metadata = readMetadata();
    if (metadata.tupleCount == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Tuple heap size has reached its maximum value.");
    }

    PageId current = metadata.firstPageId;
    PageId expectedPrevious = INVALID_PAGE_ID;
    std::unordered_set<PageId> visited;
    while (current != INVALID_PAGE_ID) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Tuple heap page chain contains a cycle.");
        }
        auto page = openOwnedPage(current);
        if (page.previousPageId() != expectedPrevious) {
            throw std::runtime_error("Tuple heap backward page link is inconsistent.");
        }
        if (page.canFit(tuple.size())) {
            const auto slotId = page.insert(tuple);
            ++metadata.tupleCount;
            writeMetadata(metadata);
            return RecordId{current, slotId};
        }
        expectedPrevious = current;
        current = page.nextPageId();
    }

    const auto newPageId = allocator_.allocatePage();
    SlottedPage::initialize(
        pager_, newPageId, metadataPageId_, INVALID_PAGE_ID, metadata.lastPageId);
    if (metadata.lastPageId == INVALID_PAGE_ID) {
        metadata.firstPageId = newPageId;
    } else {
        auto oldLast = openOwnedPage(metadata.lastPageId);
        if (oldLast.nextPageId() != INVALID_PAGE_ID) {
            throw std::runtime_error("Tuple heap metadata last page has a next link.");
        }
        oldLast.setNextPageId(newPageId);
    }
    metadata.lastPageId = newPageId;
    SlottedPage newPage(pager_, newPageId);
    const auto slotId = newPage.insert(tuple);
    ++metadata.tupleCount;
    writeMetadata(metadata);
    return RecordId{newPageId, slotId};
}

TupleBytes TupleStore::get(RecordId recordId) const {
    static_cast<void>(readMetadata());
    return openRecordPage(recordId).get(recordId.slotId);
}

bool TupleStore::tryUpdate(RecordId recordId, std::span<const std::byte> tuple) {
    validateTupleSize(tuple.size());
    static_cast<void>(readMetadata());
    return openRecordPage(recordId).tryUpdate(recordId.slotId, tuple);
}

void TupleStore::erase(RecordId recordId) {
    auto metadata = readMetadata();
    auto page = openRecordPage(recordId);
    if (!page.isOccupied(recordId.slotId)) {
        throw std::runtime_error("Tuple RecordId targets an unused slot.");
    }
    if (metadata.tupleCount == 0) {
        throw std::logic_error("Tuple heap metadata count would underflow.");
    }

    page.erase(recordId.slotId);
    if (page.empty()) {
        const auto previousPageId = page.previousPageId();
        const auto nextPageId = page.nextPageId();
        if (previousPageId == INVALID_PAGE_ID) {
            if (metadata.firstPageId != recordId.pageId) {
                throw std::runtime_error("Empty tuple page disagrees with heap first-page metadata.");
            }
            metadata.firstPageId = nextPageId;
        } else {
            auto previous = openOwnedPage(previousPageId);
            if (previous.nextPageId() != recordId.pageId) {
                throw std::runtime_error("Tuple heap previous link is inconsistent during erase.");
            }
            previous.setNextPageId(nextPageId);
        }
        if (nextPageId == INVALID_PAGE_ID) {
            if (metadata.lastPageId != recordId.pageId) {
                throw std::runtime_error("Empty tuple page disagrees with heap last-page metadata.");
            }
            metadata.lastPageId = previousPageId;
        } else {
            auto next = openOwnedPage(nextPageId);
            if (next.previousPageId() != recordId.pageId) {
                throw std::runtime_error("Tuple heap next link is inconsistent during erase.");
            }
            next.setPreviousPageId(previousPageId);
        }
        allocator_.releasePage(recordId.pageId);
    }

    --metadata.tupleCount;
    if (metadata.tupleCount == 0
        && (metadata.firstPageId != INVALID_PAGE_ID
            || metadata.lastPageId != INVALID_PAGE_ID)) {
        throw std::logic_error("Empty tuple heap retained a data page.");
    }
    writeMetadata(metadata);
}

std::vector<TupleStore::ScanEntry> TupleStore::scan() const {
    const auto metadata = readMetadata();
    if (metadata.tupleCount > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("Tuple heap is too large to materialize a scan.");
    }
    std::vector<ScanEntry> tuples;
    tuples.reserve(static_cast<std::size_t>(metadata.tupleCount));
    PageId current = metadata.firstPageId;
    PageId previous = INVALID_PAGE_ID;
    std::unordered_set<PageId> visited;
    while (current != INVALID_PAGE_ID) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Tuple heap page chain contains a cycle.");
        }
        auto page = openOwnedPage(current);
        if (page.previousPageId() != previous) {
            throw std::runtime_error("Tuple heap backward page link is inconsistent.");
        }
        for (std::size_t index = 0; index < page.slotCount(); ++index) {
            const auto slotId = static_cast<SlotId>(index);
            if (page.isOccupied(slotId)) {
                tuples.emplace_back(RecordId{current, slotId}, page.get(slotId));
            }
        }
        previous = current;
        current = page.nextPageId();
    }
    if (previous != metadata.lastPageId || tuples.size() != metadata.tupleCount) {
        throw std::runtime_error("Tuple heap scan disagrees with metadata.");
    }
    return tuples;
}

std::vector<PageId> TupleStore::reachablePageIds() const {
    validate();
    const auto metadata = readMetadata();
    std::vector<PageId> result;
    std::unordered_set<PageId> visited;
    auto current = metadata.firstPageId;
    while (current != INVALID_PAGE_ID) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Tuple heap page chain contains a cycle.");
        }
        result.push_back(current);
        current = openOwnedPage(current).nextPageId();
    }
    return result;
}

void TupleStore::validate() const {
    const auto metadata = readMetadata();
    const auto freePages = allocator_.freePageIds();
    const std::unordered_set<PageId> freePageSet(freePages.begin(), freePages.end());
    if (freePageSet.contains(metadataPageId_)) {
        throw std::runtime_error("Tuple heap metadata page is also marked free.");
    }

    std::uint64_t observedTupleCount = 0;
    PageId current = metadata.firstPageId;
    PageId previous = INVALID_PAGE_ID;
    std::unordered_set<PageId> visited;
    while (current != INVALID_PAGE_ID) {
        if (freePageSet.contains(current)) {
            throw std::runtime_error("Tuple heap data page is also marked free.");
        }
        if (!visited.insert(current).second) {
            throw std::runtime_error("Tuple heap page chain contains a cycle.");
        }
        auto page = openOwnedPage(current);
        if (page.empty()) {
            throw std::runtime_error("Tuple heap retains an empty data page.");
        }
        if (page.previousPageId() != previous) {
            throw std::runtime_error("Tuple heap backward page link is inconsistent.");
        }
        if (observedTupleCount > std::numeric_limits<std::uint64_t>::max() - page.liveCount()) {
            throw std::runtime_error("Tuple heap count overflowed during validation.");
        }
        observedTupleCount += page.liveCount();
        previous = current;
        current = page.nextPageId();
    }
    if (previous != metadata.lastPageId) {
        throw std::runtime_error("Tuple heap final page disagrees with metadata.");
    }
    if (observedTupleCount != metadata.tupleCount) {
        throw std::runtime_error("Tuple heap tuple count disagrees with reachable slots.");
    }
}

} // namespace minidb
