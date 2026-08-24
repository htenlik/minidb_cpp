#include "minidb/pager.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>

namespace minidb {

Pager::Pager(const std::string& path) : path_(path) {
    openOrCreate();

    file_.seekg(0, std::ios::end);
    const auto size = file_.tellg();
    if (size < 0) {
        throw std::runtime_error("Could not determine database file size.");
    }

    const auto bytes = static_cast<std::uint64_t>(size);
    if (bytes == 0) {
        initializeDatabase();
        return;
    }
    if (bytes % PAGE_SIZE != 0) {
        throw std::runtime_error(
            "Database file is corrupted: file size is not a multiple of PAGE_SIZE.");
    }

    const auto pageCount = bytes / PAGE_SIZE;
    if (pageCount > std::numeric_limits<PageId>::max()) {
        throw std::runtime_error("Database file contains too many pages.");
    }

    pageCount_ = static_cast<PageId>(pageCount);
    loadAndValidateDatabaseHeader();
}

Pager::~Pager() {
    try {
        flushAll();
    } catch (...) {
    }
}

void Pager::openOrCreate() {
    if (!std::filesystem::exists(path_)) {
        std::ofstream creator(path_, std::ios::binary);
        if (!creator) {
            throw std::runtime_error("Could not create database file: " + path_);
        }
    }

    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) {
        throw std::runtime_error("Could not open database file: " + path_);
    }
}

void Pager::initializeDatabase() {
    databaseHeader_ = database_format::makeCurrentDatabaseHeader();
    pageCount_ = 1;
    persistDatabaseHeader(databaseHeader_);
}

void Pager::loadAndValidateDatabaseHeader() {
    Page metadataPage{};
    file_.clear();
    file_.seekg(0, std::ios::beg);
    file_.read(
        reinterpret_cast<char*>(metadataPage.data()),
        static_cast<std::streamsize>(metadataPage.size()));
    if (file_.gcount() != static_cast<std::streamsize>(metadataPage.size())) {
        throw std::runtime_error("Failed to read database metadata page.");
    }

    databaseHeader_ = database_format::deserializeDatabaseHeader(metadataPage);
}

void Pager::persistDatabaseHeader(const database_format::DatabaseHeader& header) {
    Page metadataPage{};
    database_format::serializeDatabaseHeader(header, metadataPage);

    file_.clear();
    file_.seekp(0, std::ios::beg);
    file_.write(
        reinterpret_cast<const char*>(metadataPage.data()),
        static_cast<std::streamsize>(metadataPage.size()));
    file_.flush();
    if (!file_) {
        throw std::runtime_error("Failed to persist database metadata page.");
    }
}

void Pager::loadPageFromDisk(PageId pageId, Frame& frame) {
    file_.clear();
    file_.seekg(
        static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE),
        std::ios::beg);
    file_.read(reinterpret_cast<char*>(frame.data.data()), static_cast<std::streamsize>(PAGE_SIZE));
    if (file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        throw std::runtime_error("Failed to read full page from disk.");
    }
}

void Pager::requireDataPage(PageId pageId) {
    if (pageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Page 0 is reserved for database metadata.");
    }
}

Pager::Page& Pager::getPage(PageId pageId) {
    requireDataPage(pageId);
    if (pageId >= pageCount_) {
        throw std::out_of_range("Page ID does not exist.");
    }
    ++stats_.pageRequests;
    if (auto it = cache_.find(pageId); it != cache_.end()) {
        ++stats_.cacheHits;
        return it->second->data;
    }

    ++stats_.cacheMisses;
    auto frame = std::make_unique<Frame>();
    loadPageFromDisk(pageId, *frame);
    ++stats_.physicalPageReads;
    auto& page = frame->data;
    cache_.emplace(pageId, std::move(frame));
    return page;
}

PageId Pager::allocatePage() {
    if (pageCount_ == INVALID_PAGE_ID) {
        throw std::overflow_error("Database has exhausted the available page IDs.");
    }

    const PageId pageId = pageCount_++;
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
    if (it == cache_.end() || !it->second->dirty) {
        return;
    }

    file_.clear();
    file_.seekp(
        static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE),
        std::ios::beg);
    file_.write(
        reinterpret_cast<const char*>(it->second->data.data()),
        static_cast<std::streamsize>(PAGE_SIZE));
    if (!file_) {
        throw std::runtime_error("Failed to write page to disk.");
    }
    file_.flush();
    it->second->dirty = false;
    ++stats_.physicalPageWrites;
}

void Pager::flushAll() {
    for (const auto& entry : cache_) {
        flush(entry.first);
    }
}

void Pager::updateCatalogRootPageId(PageId pageId) {
    if (pageId != INVALID_PAGE_ID
        && (pageId == database_format::METADATA_PAGE_ID || pageId >= pageCount_)) {
        throw std::invalid_argument("Catalog root must identify an existing data page.");
    }

    auto updatedHeader = databaseHeader_;
    updatedHeader.catalogRootPageId = pageId;
    persistDatabaseHeader(updatedHeader);
    databaseHeader_ = updatedHeader;
}

void Pager::updateFreeListRootPageId(PageId pageId) {
    if (pageId != INVALID_PAGE_ID
        && (pageId == database_format::METADATA_PAGE_ID || pageId >= pageCount_)) {
        throw std::invalid_argument("Free-list root must identify an existing data page.");
    }

    auto updatedHeader = databaseHeader_;
    updatedHeader.freeListRootPageId = pageId;
    persistDatabaseHeader(updatedHeader);
    databaseHeader_ = updatedHeader;
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
