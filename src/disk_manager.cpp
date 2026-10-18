#include "minidb/disk_manager.hpp"
#include "minidb/recovery.hpp"

#include <filesystem>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unistd.h>

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
    writePageBytes(database_format::METADATA_PAGE_ID, metadataPage, true);
}

void DiskManager::requireWritable() const {
    if (writeback_.writeFailure) {
        throw std::runtime_error("Database physical write failed; restart is required");
    }
}

void DiskManager::requireWriteEpochAvailable() const {
    requireWritable();
    if (writeback_.physicalWriteEpoch == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Database physical write epoch is exhausted");
    }
}

void DiskManager::writePageBytes(PageId pageId, const Page& page, bool flushStream) {
    // All callers hold ioLatch_ (initialization runs before publication). Check
    // exhaustion before touching the file; count only accepted complete writes.
    requireWriteEpochAvailable();
    try {
        file_.clear();
        file_.seekp(static_cast<std::streamoff>(pageId)
            * static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);
        file_.write(reinterpret_cast<const char*>(page.data()),
                    static_cast<std::streamsize>(page.size()));
        if (flushStream) file_.flush();
        if (!file_) throw std::runtime_error("Failed to write database page");
        ++writeback_.physicalWriteEpoch;
    } catch (...) {
        writeback_.writeFailure = true;
        throw;
    }
}

void DiskManager::requireExistingDataPage(PageId pageId) const {
    if (pageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Page 0 is reserved for database metadata.");
    }
    if (pageId == INVALID_PAGE_ID || pageId >= pageCount_) {
        throw std::out_of_range("Page ID does not exist.");
    }
}

void DiskManager::requireExistingPhysicalPage(PageId pageId) const {
    if (pageId == INVALID_PAGE_ID || pageId >= pageCount_) {
        throw std::out_of_range("Physical PageId does not exist.");
    }
}

void DiskManager::readPage(PageId pageId, Page& output) {
    std::lock_guard lock(ioLatch_);
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
    std::lock_guard lock(ioLatch_);
    requireExistingDataPage(pageId);
    writePageBytes(pageId, page, true);
}

PageId DiskManager::appendPage() {
    std::lock_guard lock(ioLatch_);
    return appendPage(pageCount_);
}

PageId DiskManager::appendPage(PageId expectedPageId) {
    std::lock_guard lock(ioLatch_);
    if (expectedPageId != pageCount_) {
        throw std::logic_error("Physical extent changed after page reservation");
    }
    if (pageCount_ == INVALID_PAGE_ID) {
        throw std::overflow_error("Database has exhausted the available page IDs.");
    }
    const auto pageId = pageCount_;
    Page page{};
    writePageBytes(pageId, page, true);
    ++pageCount_;
    return pageId;
}

void DiskManager::flush() {
    std::lock_guard lock(ioLatch_);
    requireWritable();
    file_.flush();
    if (!file_) {
        writeback_.writeFailure = true;
        throw std::runtime_error("Failed to flush database file.");
    }
}

void DiskManager::sync() {
    std::lock_guard lock(ioLatch_);
    try {
        flush();
        const auto descriptor = ::open(path_.c_str(), O_RDWR);
        if (descriptor < 0) {
            throw std::runtime_error(
                "Could not open database for fsync: " + std::string(std::strerror(errno)));
        }
        int result;
        int savedError;
        try {
            recoveryFailPoint("database_before_fsync");
            result = syncFunction_ ? syncFunction_(descriptor) : ::fsync(descriptor);
            savedError = errno;
            recoveryFailPoint("database_after_fsync_syscall");
        } catch (...) {
            ::close(descriptor);
            throw;
        }
        ::close(descriptor);
        if (result != 0) {
            throw std::runtime_error(
                "Could not fsync database: " + std::string(std::strerror(savedError)));
        }
        writeback_.synchronizedWriteEpoch = writeback_.physicalWriteEpoch;
        writeback_.initialSyncRequired = false;
        ++writeback_.databaseSyncsCompleted;
    } catch (...) {
        ++writeback_.databaseSyncFailures;
        throw;
    }
    recoveryFailPoint("database_after_fsync");
}

bool DiskManager::syncPendingWritebacks() {
    std::lock_guard lock(ioLatch_);
    requireWritable();
    if (!writeback_.pending()) return false;
    sync();
    return true;
}

PhysicalWritebackStats DiskManager::writebackStats() const noexcept {
    std::lock_guard lock(ioLatch_);
    return writeback_;
}

void DiskManager::readPhysicalPage(PageId pageId, Page& output) {
    std::lock_guard lock(ioLatch_);
    requireExistingPhysicalPage(pageId);
    file_.clear();
    file_.seekg(
        static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE),
        std::ios::beg);
    file_.read(reinterpret_cast<char*>(output.data()),
               static_cast<std::streamsize>(output.size()));
    if (file_.gcount() != static_cast<std::streamsize>(output.size())) {
        throw std::runtime_error("Failed to read full physical page from disk.");
    }
}

void DiskManager::writePhysicalPage(PageId pageId, const Page& page) {
    std::lock_guard lock(ioLatch_);
    if (pageId == INVALID_PAGE_ID) {
        throw std::invalid_argument("Cannot physically write INVALID_PAGE_ID.");
    }
    auto updatedHeader = databaseHeader_;
    if (pageId == database_format::METADATA_PAGE_ID) {
        updatedHeader = database_format::deserializeDatabaseHeader(page);
    }
    while (pageCount_ <= pageId) {
        static_cast<void>(appendPage());
    }
    writePageBytes(pageId, page, false);
    if (pageId == database_format::METADATA_PAGE_ID) {
        databaseHeader_ = updatedHeader;
    }
}

void DiskManager::reopenFile() {
    file_.close();
    file_.clear();
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw std::runtime_error("Could not reopen database file: " + path_);
}

void DiskManager::truncateToPageCount(std::uint64_t pageCount) {
    std::lock_guard lock(ioLatch_);
    if (pageCount == 0 || pageCount > INVALID_PAGE_ID) {
        throw std::invalid_argument("Recovery page count is outside the supported range.");
    }
    if (pageCount == pageCount_) return;
    requireWriteEpochAvailable();
    flush();
    const auto descriptor = ::open(path_.c_str(), O_RDWR);
    if (descriptor < 0) {
        throw std::runtime_error(
            "Could not open database for truncation: " + std::string(std::strerror(errno)));
    }
    const auto byteCount = pageCount * PAGE_SIZE;
    const auto result = ::ftruncate(descriptor, static_cast<off_t>(byteCount));
    const auto savedError = errno;
    ::close(descriptor);
    if (result != 0) {
        writeback_.writeFailure = true;
        throw std::runtime_error(
            "Could not truncate database: " + std::string(std::strerror(savedError)));
    }
    ++writeback_.physicalWriteEpoch;
    reopenFile();
    pageCount_ = static_cast<PageId>(pageCount);
    loadAndValidateDatabaseHeader();
}

void DiskManager::reloadDatabaseHeader() {
    std::lock_guard lock(ioLatch_);
    loadAndValidateDatabaseHeader();
}

void DiskManager::updateCatalogRootPageId(PageId pageId) {
    std::lock_guard lock(ioLatch_);
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
    std::lock_guard lock(ioLatch_);
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
