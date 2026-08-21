#include "minidb/record_store.hpp"

#include <stdexcept>
#include <unordered_set>

namespace minidb {

RecordStore::RecordStore(Pager& pager) : RecordStore(pager, INVALID_PAGE_ID) {}

RecordStore::RecordStore(Pager& pager, PageId headPageId)
    : pager_(pager), headPageId_(headPageId) {
    validateHeadPageId();
    validateChain();
}

RecordId RecordStore::insert(const Row& row) {
    const auto serialized = serializeRow(row);

    if (empty()) {
        headPageId_ = allocateRecordPage();
        RecordPage page(pager_, headPageId_);
        return RecordId{headPageId_, page.insert(serialized)};
    }

    std::unordered_set<PageId> visited;
    PageId currentPageId = headPageId_;
    while (currentPageId != INVALID_PAGE_ID) {
        if (!visited.insert(currentPageId).second) {
            throw std::runtime_error("Record page chain contains a cycle.");
        }

        RecordPage page(pager_, currentPageId);
        if (page.hasFreeSlot()) {
            return RecordId{currentPageId, page.insert(serialized)};
        }

        const auto nextPageId = page.nextPageId();
        if (nextPageId != INVALID_PAGE_ID) {
            currentPageId = nextPageId;
            continue;
        }

        const auto newPageId = allocateRecordPage();
        page.setNextPageId(newPageId);
        RecordPage newPage(pager_, newPageId);
        return RecordId{newPageId, newPage.insert(serialized)};
    }

    throw std::runtime_error("Record page chain ended unexpectedly.");
}

Row RecordStore::get(RecordId recordId) {
    auto page = findRecordPage(recordId);
    return deserializeRow(page.get(recordId.slotId));
}

void RecordStore::update(RecordId recordId, const Row& row) {
    const auto serialized = serializeRow(row);
    auto page = findRecordPage(recordId);
    page.update(recordId.slotId, serialized);
}

void RecordStore::erase(RecordId recordId) {
    auto page = findRecordPage(recordId);
    page.erase(recordId.slotId);
}

std::vector<RecordStore::ScanEntry> RecordStore::scan() {
    std::vector<ScanEntry> records;
    std::unordered_set<PageId> visited;

    PageId currentPageId = headPageId_;
    while (currentPageId != INVALID_PAGE_ID) {
        if (!visited.insert(currentPageId).second) {
            throw std::runtime_error("Record page chain contains a cycle.");
        }

        RecordPage page(pager_, currentPageId);
        for (std::size_t index = 0; index < record_page_layout::SLOT_CAPACITY; ++index) {
            const auto slotId = static_cast<SlotId>(index);
            if (!page.isOccupied(slotId)) {
                continue;
            }
            records.emplace_back(
                RecordId{currentPageId, slotId},
                deserializeRow(page.get(slotId)));
        }

        currentPageId = page.nextPageId();
    }

    return records;
}

PageId RecordStore::allocateRecordPage() {
    const auto pageId = pager_.allocatePage();
    RecordPage::initialize(pager_, pageId);
    return pageId;
}

RecordPage RecordStore::findRecordPage(RecordId recordId) {
    validateRecordId(recordId);

    std::unordered_set<PageId> visited;
    PageId currentPageId = headPageId_;
    while (currentPageId != INVALID_PAGE_ID) {
        if (!visited.insert(currentPageId).second) {
            throw std::runtime_error("Record page chain contains a cycle.");
        }

        RecordPage page(pager_, currentPageId);
        if (currentPageId == recordId.pageId) {
            return page;
        }
        currentPageId = page.nextPageId();
    }

    throw std::out_of_range("Record ID page does not belong to this record store.");
}

void RecordStore::validateHeadPageId() const {
    if (headPageId_ == INVALID_PAGE_ID) {
        return;
    }
    if (headPageId_ == database_format::METADATA_PAGE_ID
        || headPageId_ >= pager_.pageCount()) {
        throw std::out_of_range("Record store head page ID does not exist.");
    }
}

void RecordStore::validateChain() {
    std::unordered_set<PageId> visited;
    PageId currentPageId = headPageId_;
    while (currentPageId != INVALID_PAGE_ID) {
        if (!visited.insert(currentPageId).second) {
            throw std::runtime_error("Record page chain contains a cycle.");
        }

        RecordPage page(pager_, currentPageId);
        currentPageId = page.nextPageId();
    }
}

void RecordStore::validateRecordId(RecordId recordId) const {
    if (!recordId.isValid()
        || recordId.pageId >= pager_.pageCount()) {
        throw std::out_of_range("Record ID contains an invalid page ID.");
    }
    if (recordId.slotId >= record_page_layout::SLOT_CAPACITY) {
        throw std::out_of_range("Record ID contains an invalid slot ID.");
    }
}

} // namespace minidb
