#include "minidb/tuple_store.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/page_access.hpp"
#include "minidb/storage_error.hpp"
#include "minidb/page_lsn.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace minidb {

TupleStore TupleStore::create(
    BufferPoolManager& bufferPool,
    DiskManager& diskManager,
    PageAllocator& allocator) {
    const auto metadataPageId = allocator.allocatePage();
    TupleStore store(bufferPool, diskManager, allocator, metadataPageId);
    store.writeMetadata(Metadata{});
    return store;
}

TupleStore TupleStore::open(
    BufferPoolManager& bufferPool,
    DiskManager& diskManager,
    PageAllocator& allocator,
    PageId metadataPageId) {
    TupleStore store(bufferPool, diskManager, allocator, metadataPageId);
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
        || metadataPageId_ >= diskManager_.pageCount()) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            "Tuple heap metadata page ID does not exist.");
    }
}

TupleStore::Metadata TupleStore::readMetadata() const {
    validateMetadataPageId();
    const auto guard = requireReadPage(bufferPool_, metadataPageId_, "read tuple heap metadata");
    const auto page = guard.data();
    using namespace tuple_heap_metadata_layout;
    if (!std::equal(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET)) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Invalid tuple heap metadata magic/type.");
    }
    const auto version = byte_codec::readUint32(page, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Unsupported tuple heap metadata version " + std::to_string(version) + ".");
    }
    if (byte_codec::readUint32(page, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap metadata has an invalid header size.");
    }
    if (!std::all_of(
            page.begin() + PAGE_LSN_OFFSET + PAGE_LSN_SIZE,
            page.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap metadata reserved bytes are not zero.");
    }
    static_cast<void>(readPersistentPageLsn(page));

    Metadata metadata{
        byte_codec::readUint32(page, FIRST_PAGE_ID_OFFSET),
        byte_codec::readUint32(page, LAST_PAGE_ID_OFFSET),
        byte_codec::readUint64(page, TUPLE_COUNT_OFFSET),
    };
    const auto validPageReference = [&](PageId pageId) {
        return pageId != database_format::METADATA_PAGE_ID
            && pageId != INVALID_PAGE_ID
            && pageId != metadataPageId_
            && pageId < diskManager_.pageCount();
    };
    if (metadata.tupleCount == 0) {
        if (metadata.firstPageId != INVALID_PAGE_ID
            || metadata.lastPageId != INVALID_PAGE_ID) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Empty tuple heap metadata retains data-page links.");
        }
    } else if (!validPageReference(metadata.firstPageId)
               || !validPageReference(metadata.lastPageId)) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Nonempty tuple heap metadata has invalid first/last pages.");
    }
    if ((metadata.firstPageId == INVALID_PAGE_ID)
        != (metadata.lastPageId == INVALID_PAGE_ID)) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap metadata first/last page state disagrees.");
    }
    return metadata;
}

void TupleStore::writeMetadata(const Metadata& metadata) {
    validateMetadataPageId();
    auto guard = requireWritePage(bufferPool_, metadataPageId_, "write tuple heap metadata");
    auto page = guard.data();
    using namespace tuple_heap_metadata_layout;
    std::fill(page.begin(), page.end(), std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    byte_codec::writeUint32(page, FIRST_PAGE_ID_OFFSET, metadata.firstPageId);
    byte_codec::writeUint32(page, LAST_PAGE_ID_OFFSET, metadata.lastPageId);
    byte_codec::writeUint64(page, TUPLE_COUNT_OFFSET, metadata.tupleCount);
}

void TupleStore::validateTupleSize(std::size_t size) {
    if (size == 0) {
        throw std::invalid_argument("Zero-length tuples are not supported.");
    }
    if (size > slotted_page_layout::MAX_TUPLE_SIZE) {
        throw std::invalid_argument("Tuple exceeds the maximum inline slotted-page size.");
    }
}

void TupleStore::validateOwnedPage(const ConstSlottedPageView& page) const {
    if (page.heapMetadataPageId() != metadataPageId_) {
        throw std::out_of_range("Slotted page belongs to a different tuple heap.");
    }
}

void TupleStore::validateRecordId(
    RecordId recordId,
    const ConstSlottedPageView& page) const {
    if (!recordId.isValid() || recordId.pageId >= diskManager_.pageCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple-page ID.");
    }
    validateOwnedPage(page);
    if (recordId.slotId >= page.slotCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple SlotId.");
    }
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
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Tuple heap page chain contains a cycle.");
        }
        PageId nextPageId;
        bool selected;
        {
            const auto guard = requireReadPage(bufferPool_, current, "scan tuple heap for insert");
            const ConstSlottedPageView page(guard.data(), current, diskManager_.pageCount());
            validateOwnedPage(page);
            if (page.previousPageId() != expectedPrevious) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap backward page link is inconsistent.");
            }
            selected = page.canFit(tuple.size());
            nextPageId = page.nextPageId();
        }
        if (selected) {
            auto guard = requireWritePage(bufferPool_, current, "insert tuple into heap page");
            SlottedPageView page(guard.data(), current, diskManager_.pageCount());
            validateOwnedPage(page);
            if (!page.canFit(tuple.size())) {
                throw std::logic_error("Selected tuple page lost free space without concurrency.");
            }
            const auto slotId = page.insert(tuple);
            guard.drop();
            ++metadata.tupleCount;
            writeMetadata(metadata);
            return RecordId{current, slotId};
        }
        expectedPrevious = current;
        current = nextPageId;
    }

    const auto newPageId = allocator_.allocatePage();
    SlotId slotId;
    {
        auto guard = requireWritePage(bufferPool_, newPageId, "initialize tuple heap page");
        SlottedPageView::initialize(
            guard.data(),
            newPageId,
            diskManager_.pageCount(),
            metadataPageId_,
            INVALID_PAGE_ID,
            metadata.lastPageId);
        SlottedPageView page(guard.data(), newPageId, diskManager_.pageCount());
        slotId = page.insert(tuple);
    }
    if (metadata.lastPageId == INVALID_PAGE_ID) {
        metadata.firstPageId = newPageId;
    } else {
        auto guard = requireWritePage(bufferPool_, metadata.lastPageId, "link tuple heap page");
        SlottedPageView oldLast(
            guard.data(), metadata.lastPageId, diskManager_.pageCount());
        validateOwnedPage(oldLast);
        if (oldLast.nextPageId() != INVALID_PAGE_ID) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Tuple heap metadata last page has a next link.");
        }
        oldLast.setNextPageId(newPageId);
    }
    metadata.lastPageId = newPageId;
    ++metadata.tupleCount;
    writeMetadata(metadata);
    return RecordId{newPageId, slotId};
}

TupleBytes TupleStore::get(RecordId recordId) const {
    static_cast<void>(readMetadata());
    if (!recordId.isValid() || recordId.pageId >= diskManager_.pageCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple-page ID.");
    }
    const auto guard = requireReadPage(bufferPool_, recordId.pageId, "get tuple");
    const ConstSlottedPageView page(
        guard.data(), recordId.pageId, diskManager_.pageCount());
    validateRecordId(recordId, page);
    return page.get(recordId.slotId);
}

bool TupleStore::tryUpdate(RecordId recordId, std::span<const std::byte> tuple) {
    validateTupleSize(tuple.size());
    static_cast<void>(readMetadata());
    if (!recordId.isValid() || recordId.pageId >= diskManager_.pageCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple-page ID.");
    }
    auto guard = requireWritePage(bufferPool_, recordId.pageId, "update tuple");
    SlottedPageView page(guard.data(), recordId.pageId, diskManager_.pageCount());
    validateRecordId(recordId, page);
    return page.tryUpdate(recordId.slotId, tuple);
}

void TupleStore::erase(RecordId recordId) {
    auto metadata = readMetadata();
    if (!recordId.isValid() || recordId.pageId >= diskManager_.pageCount()) {
        throw std::out_of_range("RecordId contains an invalid tuple-page ID.");
    }

    PageId previousPageId;
    PageId nextPageId;
    bool becameEmpty;
    {
        auto guard = requireWritePage(bufferPool_, recordId.pageId, "erase tuple");
        SlottedPageView page(guard.data(), recordId.pageId, diskManager_.pageCount());
        validateRecordId(recordId, page);
        if (!page.isOccupied(recordId.slotId)) {
            throw std::runtime_error("Tuple RecordId targets an unused slot.");
        }
        if (metadata.tupleCount == 0) {
            throw std::logic_error("Tuple heap metadata count would underflow.");
        }
        previousPageId = page.previousPageId();
        nextPageId = page.nextPageId();
        page.erase(recordId.slotId);
        becameEmpty = page.empty();
    }

    if (becameEmpty) {
        if (previousPageId == INVALID_PAGE_ID) {
            if (metadata.firstPageId != recordId.pageId) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Empty tuple page disagrees with heap first-page metadata.");
            }
            metadata.firstPageId = nextPageId;
        } else {
            auto guard = requireWritePage(bufferPool_, previousPageId, "unlink previous heap page");
            SlottedPageView previous(guard.data(), previousPageId, diskManager_.pageCount());
            validateOwnedPage(previous);
            if (previous.nextPageId() != recordId.pageId) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap previous link is inconsistent during erase.");
            }
            previous.setNextPageId(nextPageId);
        }
        if (nextPageId == INVALID_PAGE_ID) {
            if (metadata.lastPageId != recordId.pageId) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Empty tuple page disagrees with heap last-page metadata.");
            }
            metadata.lastPageId = previousPageId;
        } else {
            auto guard = requireWritePage(bufferPool_, nextPageId, "unlink next heap page");
            SlottedPageView next(guard.data(), nextPageId, diskManager_.pageCount());
            validateOwnedPage(next);
            if (next.previousPageId() != recordId.pageId) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap next link is inconsistent during erase.");
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
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Tuple heap page chain contains a cycle.");
        }
        PageId nextPageId;
        {
            const auto guard = requireReadPage(bufferPool_, current, "scan tuple heap page");
            const ConstSlottedPageView page(guard.data(), current, diskManager_.pageCount());
            validateOwnedPage(page);
            if (page.previousPageId() != previous) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap backward page link is inconsistent.");
            }
            for (std::size_t index = 0; index < page.slotCount(); ++index) {
                const auto slotId = static_cast<SlotId>(index);
                if (page.isOccupied(slotId)) {
                    tuples.emplace_back(RecordId{current, slotId}, page.get(slotId));
                }
            }
            nextPageId = page.nextPageId();
        }
        previous = current;
        current = nextPageId;
    }
    if (previous != metadata.lastPageId || tuples.size() != metadata.tupleCount) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap scan disagrees with metadata.");
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
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Tuple heap page chain contains a cycle.");
        }
        result.push_back(current);
        const auto guard = requireReadPage(bufferPool_, current, "read reachable tuple page");
        const ConstSlottedPageView page(guard.data(), current, diskManager_.pageCount());
        validateOwnedPage(page);
        current = page.nextPageId();
    }
    return result;
}

void TupleStore::validate() const {
    const auto metadata = readMetadata();
    const auto freePages = allocator_.freePageIds();
    const std::unordered_set<PageId> freePageSet(freePages.begin(), freePages.end());
    if (freePageSet.contains(metadataPageId_)) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap metadata page is also marked free.");
    }

    std::uint64_t observedTupleCount = 0;
    PageId current = metadata.firstPageId;
    PageId previous = INVALID_PAGE_ID;
    std::unordered_set<PageId> visited;
    while (current != INVALID_PAGE_ID) {
        if (freePageSet.contains(current)) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Tuple heap data page is also marked free.");
        }
        if (!visited.insert(current).second) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Tuple heap page chain contains a cycle.");
        }
        PageId nextPageId;
        {
            const auto guard = requireReadPage(bufferPool_, current, "validate tuple heap page");
            const ConstSlottedPageView page(guard.data(), current, diskManager_.pageCount());
            validateOwnedPage(page);
            if (page.empty()) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap retains an empty data page.");
            }
            if (page.previousPageId() != previous) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap backward page link is inconsistent.");
            }
            if (observedTupleCount > std::numeric_limits<std::uint64_t>::max() - page.liveCount()) {
                throw StorageError(
                    StorageErrorKind::CorruptPage,
                    "Tuple heap count overflowed during validation.");
            }
            observedTupleCount += page.liveCount();
            nextPageId = page.nextPageId();
        }
        previous = current;
        current = nextPageId;
    }
    if (previous != metadata.lastPageId) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap final page disagrees with metadata.");
    }
    if (observedTupleCount != metadata.tupleCount) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Tuple heap tuple count disagrees with reachable slots.");
    }
}

} // namespace minidb
