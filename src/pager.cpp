#include "minidb/pager.hpp"

#include <stdexcept>

namespace minidb {

Pager::Pager(const std::string& path) : diskManager_(path) {}

Pager::~Pager() {
    try {
        flushAll();
    } catch (...) {
    }
}

void Pager::requireDataPage(PageId pageId) {
    if (pageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Page 0 is reserved for database metadata.");
    }
}

Pager::Page& Pager::getPage(PageId pageId) {
    requireDataPage(pageId);
    if (pageId >= pageCount()) throw std::out_of_range("Page ID does not exist.");
    ++stats_.pageRequests;
    if (auto it = cache_.find(pageId); it != cache_.end()) {
        ++stats_.cacheHits;
        return it->second->data;
    }

    ++stats_.cacheMisses;
    auto frame = std::make_unique<Frame>();
    diskManager_.readPage(pageId, frame->data);
    ++stats_.physicalPageReads;
    auto& page = frame->data;
    cache_.emplace(pageId, std::move(frame));
    return page;
}

PageId Pager::allocatePage() {
    const auto pageId = diskManager_.appendPage();
    auto frame = std::make_unique<Frame>();
    frame->data.fill(std::byte{0});
    frame->dirty = true;
    cache_.emplace(pageId, std::move(frame));
    ++stats_.appendedPages;
    return pageId;
}

void Pager::markDirty(PageId pageId) {
    requireDataPage(pageId);
    auto it = cache_.find(pageId);
    if (it == cache_.end()) {
        throw std::runtime_error("Cannot mark a page dirty before loading it.");
    }
    it->second->dirty = true;
    ++stats_.dirtyMarks;
}

void Pager::flush(PageId pageId) {
    requireDataPage(pageId);
    ++stats_.flushCalls;
    auto it = cache_.find(pageId);
    if (it == cache_.end() || !it->second->dirty) return;
    diskManager_.writePage(pageId, it->second->data);
    it->second->dirty = false;
    ++stats_.physicalPageWrites;
}

void Pager::flushAll() {
    for (const auto& entry : cache_) flush(entry.first);
}

void Pager::updateCatalogRootPageId(PageId pageId) {
    diskManager_.updateCatalogRootPageId(pageId);
}

void Pager::updateFreeListRootPageId(PageId pageId) {
    diskManager_.updateFreeListRootPageId(pageId);
}

PagerStats Pager::stats() const noexcept {
    auto result = stats_;
    result.residentPages = cache_.size();
    return result;
}

void Pager::resetStats() noexcept {
    stats_ = {};
}

} // namespace minidb
