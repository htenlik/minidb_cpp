#include "minidb/disk_manager.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>

namespace minidb {

DiskManager::DiskManager(const std::string& path) : path_(path) {
    openOrCreate();

    file_.seekg(0, std::ios::end);
    const auto size = file_.tellg();
    if (size < 0) throw std::runtime_error("Could not determine database file size.");

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

void DiskManager::openOrCreate() {
    if (!std::filesystem::exists(path_)) {
        std::ofstream creator(path_, std::ios::binary);
        if (!creator) throw std::runtime_error("Could not create database file: " + path_);
    }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw std::runtime_error("Could not open database file: " + path_);
}

void DiskManager::initializeDatabase() {
    databaseHeader_ = database_format::makeCurrentDatabaseHeader();
    pageCount_ = 1;
    persistDatabaseHeader(databaseHeader_);
}

void DiskManager::loadAndValidateDatabaseHeader() {
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

void DiskManager::persistDatabaseHeader(const database_format::DatabaseHeader& header) {
    Page metadataPage{};
    database_format::serializeDatabaseHeader(header, metadataPage);
    file_.clear();
    file_.seekp(0, std::ios::beg);
    file_.write(
        reinterpret_cast<const char*>(metadataPage.data()),
        static_cast<std::streamsize>(metadataPage.size()));
    file_.flush();
    if (!file_) throw std::runtime_error("Failed to persist database metadata page.");
}

void DiskManager::requireExistingDataPage(PageId pageId) const {
    if (pageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Page 0 is reserved for database metadata.");
    }
    if (pageId == INVALID_PAGE_ID || pageId >= pageCount_) {
        throw std::out_of_range("Page ID does not exist.");
    }
}

void DiskManager::readPage(PageId pageId, Page& output) {
    requireExistingDataPage(pageId);
    file_.clear();
    file_.seekg(
        static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE),
        std::ios::beg);
    file_.read(
        reinterpret_cast<char*>(output.data()),
        static_cast<std::streamsize>(output.size()));
    if (file_.gcount() != static_cast<std::streamsize>(output.size())) {
        throw std::runtime_error("Failed to read full page from disk.");
    }
}

void DiskManager::writePage(PageId pageId, const Page& page) {
    requireExistingDataPage(pageId);
    file_.clear();
    file_.seekp(
        static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE),
        std::ios::beg);
    file_.write(
        reinterpret_cast<const char*>(page.data()),
        static_cast<std::streamsize>(page.size()));
    file_.flush();
    if (!file_) throw std::runtime_error("Failed to write page to disk.");
}

PageId DiskManager::appendPage() {
    if (pageCount_ == INVALID_PAGE_ID) {
        throw std::overflow_error("Database has exhausted the available page IDs.");
    }
    const auto pageId = pageCount_;
    Page page{};
    file_.clear();
    file_.seekp(0, std::ios::end);
    file_.write(
        reinterpret_cast<const char*>(page.data()),
        static_cast<std::streamsize>(page.size()));
    file_.flush();
    if (!file_) throw std::runtime_error("Failed to append database page.");
    ++pageCount_;
    return pageId;
}

void DiskManager::flush() {
    file_.flush();
    if (!file_) throw std::runtime_error("Failed to flush database file.");
}

void DiskManager::updateCatalogRootPageId(PageId pageId) {
    if (pageId != INVALID_PAGE_ID
        && (pageId == database_format::METADATA_PAGE_ID || pageId >= pageCount_)) {
        throw std::invalid_argument("Catalog root must identify an existing data page.");
    }
    auto updatedHeader = databaseHeader_;
    updatedHeader.catalogRootPageId = pageId;
    persistDatabaseHeader(updatedHeader);
    databaseHeader_ = updatedHeader;
}

void DiskManager::updateFreeListRootPageId(PageId pageId) {
    if (pageId != INVALID_PAGE_ID
        && (pageId == database_format::METADATA_PAGE_ID || pageId >= pageCount_)) {
        throw std::invalid_argument("Free-list root must identify an existing data page.");
    }
    auto updatedHeader = databaseHeader_;
    updatedHeader.freeListRootPageId = pageId;
    persistDatabaseHeader(updatedHeader);
    databaseHeader_ = updatedHeader;
}

} // namespace minidb
