#include "minidb/log_manager.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/recovery.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace minidb {
namespace {

[[noreturn]] void throwIo(const std::string& operation) {
    throw WalError(WalErrorKind::Io, operation + ": " + std::strerror(errno));
}

std::uint64_t descriptorSize(int descriptor) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) throwIo("Could not inspect WAL file");
    if (status.st_size < 0) throw WalError(WalErrorKind::Io, "WAL file has negative size");
    return static_cast<std::uint64_t>(status.st_size);
}

} // namespace

LogManager::LogManager(
    std::string walPath,
    std::size_t bufferCapacity,
    LogOpenMode openMode,
    WalStorageMode storageMode,
    std::uint32_t segmentPayloadCapacity)
    : path_(std::move(walPath)),
      segmentedPath_(segmentedWalPathForLegacyWal(path_)),
      bufferCapacity_(bufferCapacity), openMode_(openMode),
      requestedStorageMode_(storageMode), segmentPayloadCapacity_(segmentPayloadCapacity) {
    if (path_.empty()) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL path must not be empty");
    }
    if (bufferCapacity_ == 0) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL buffer capacity must be positive");
    }
    buffer_.reserve(bufferCapacity_);
    openOrCreate();
}

LogManager::~LogManager() {
    if (segmented_ != nullptr) {
        try { flushAll(); } catch (...) {}
        return;
    }
    if (descriptor_ < 0) return;
    try {
        flushAll();
    } catch (...) {
    }
    ::close(descriptor_);
}

void LogManager::openOrCreate() {
    if (requestedStorageMode_ == WalStorageMode::Auto
        && !std::filesystem::exists(segmentedPath_)) {
        std::error_code cleanupError;
        std::filesystem::remove_all(segmentedPath_ + ".tmp", cleanupError);
    }
    if (requestedStorageMode_ == WalStorageMode::Segmented
        || (requestedStorageMode_ == WalStorageMode::Auto
            && std::filesystem::exists(segmentedPath_))
        || (requestedStorageMode_ == WalStorageMode::Auto
            && !std::filesystem::exists(path_))) {
        openSegmented();
        return;
    }
    activeStorageMode_ = WalStorageMode::LegacySingleFile;
    descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (descriptor_ < 0) throwIo("Could not open WAL file");
    try {
        if (descriptorSize(descriptor_) == 0) initializeNewWal();
        else if (openMode_ == LogOpenMode::DeferredRecovery) loadExistingWalDeferred();
        else loadExistingWal();
    } catch (...) {
        ::close(descriptor_);
        descriptor_ = -1;
        throw;
    }
}

void LogManager::openSegmented() {
    activeStorageMode_ = WalStorageMode::Segmented;
    segmented_ = std::make_unique<SegmentedWalStorage>(
        segmentedPath_, segmentPayloadCapacity_, true);
    const auto scanned = segmented_->scan();
    nextLsn_ = scanned.validBytes;
    bufferStartOffset_ = nextLsn_;
    truncatedTail_ = scanned.truncatedTail;
    if (openMode_ == LogOpenMode::DeferredRecovery) {
        recoveryPending_ = true;
        return;
    }
    for (const auto& record : scanned.records) knownLsns_.insert(record.lsn);
    if (!scanned.records.empty()) {
        lastAppendedLsn_ = scanned.records.back().lsn;
        durableLsn_ = lastAppendedLsn_;
    }
}

void LogManager::loadExistingWalDeferred() {
    const auto size = descriptorSize(descriptor_);
    if (size < wal_file_layout::HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL file header is truncated");
    }
    std::array<std::byte, wal_file_layout::HEADER_SIZE> header{};
    std::size_t completed = 0;
    while (completed < header.size()) {
        const auto count = ::pread(descriptor_, header.data() + completed,
                                   header.size() - completed,
                                   static_cast<off_t>(completed));
        if (count < 0) { if (errno == EINTR) continue; throwIo("Could not read WAL header"); }
        if (count == 0) throw WalError(WalErrorKind::CorruptHeader, "WAL header is truncated");
        completed += static_cast<std::size_t>(count);
    }
    validateWalFileHeader(header);
    nextLsn_ = size;
    bufferStartOffset_ = nextLsn_;
    recoveryPending_ = true;
}

void LogManager::initializeNewWal() {
    const auto header = encodeWalFileHeader();
    writeBytes(0, header);
    syncWal();
    nextLsn_ = wal_file_layout::HEADER_SIZE;
    bufferStartOffset_ = nextLsn_;
}

void LogManager::loadExistingWal() {
    const auto scanned = scanWalFile(path_);
    nextLsn_ = scanned.validBytes;
    bufferStartOffset_ = nextLsn_;
    truncatedTail_ = scanned.truncatedTail;
    for (const auto& record : scanned.records) knownLsns_.insert(record.lsn);
    if (!scanned.records.empty()) {
        lastAppendedLsn_ = scanned.records.back().lsn;
        durableLsn_ = lastAppendedLsn_;
    }
}

void LogManager::writeBytes(
    std::uint64_t offset,
    std::span<const std::byte> bytes) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
        || bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset) {
        throw WalError(WalErrorKind::Io, "WAL file offset exceeds platform limits");
    }
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const auto result = ::pwrite(
            descriptor_,
            bytes.data() + completed,
            bytes.size() - completed,
            static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) continue;
            throwIo("Could not write WAL file");
        }
        if (result == 0) {
            throw WalError(WalErrorKind::Io, "WAL write made no progress");
        }
        completed += static_cast<std::size_t>(result);
        ++stats_.physicalWrites;
        stats_.bytesWritten += static_cast<std::uint64_t>(result);
    }
}

void LogManager::syncWal() {
    if (::fsync(descriptor_) != 0) {
        throw WalError(
            WalErrorKind::Durability,
            "Could not fsync WAL file: " + std::string(std::strerror(errno)));
    }
    ++stats_.fsyncCalls;
}

void LogManager::writeBuffer() {
    if (buffer_.empty()) return;
    if (segmented_ != nullptr) {
        const auto before = segmented_->stats();
        std::size_t offset = 0;
        while (offset < buffer_.size()) {
            const auto total = byte_codec::readUint32(
                std::span<const std::byte>(buffer_).subspan(offset),
                wal_record_layout::TOTAL_LENGTH_OFFSET);
            segmented_->appendRecord(
                bufferStartOffset_ + offset,
                std::span<const std::byte>(buffer_).subspan(offset, total));
            offset += total;
        }
        const auto after = segmented_->stats();
        stats_.physicalWrites += after.physicalWrites - before.physicalWrites;
        stats_.bytesWritten += after.bytesWritten - before.bytesWritten;
    } else {
        writeBytes(bufferStartOffset_, buffer_);
    }
    bufferStartOffset_ += buffer_.size();
    buffer_.clear();
    ++stats_.bufferFlushes;
}

Lsn LogManager::append(LogRecord record) {
    if (recoveryPending_) {
        throw WalError(WalErrorKind::InvalidArgument,
                       "Cannot append before deferred startup recovery completes");
    }
    if (truncatedTail_) {
        throw WalError(
            WalErrorKind::TruncatedTail,
            "Cannot append until the detected WAL tail is explicitly truncated");
    }
    const auto lsn = nextLsn_;
    const auto encoded = encodeWalRecord(record, lsn);
    if (nextLsn_ > std::numeric_limits<Lsn>::max() - encoded.size()
        || nextLsn_ + encoded.size() == INVALID_LSN) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL LSN space is exhausted");
    }

    if (encoded.size() > bufferCapacity_) {
        writeBuffer();
        if (segmented_ != nullptr) {
            const auto before = segmented_->stats();
            segmented_->appendRecord(lsn, encoded);
            const auto after = segmented_->stats();
            stats_.physicalWrites += after.physicalWrites - before.physicalWrites;
            stats_.bytesWritten += after.bytesWritten - before.bytesWritten;
        } else {
            writeBytes(lsn, encoded);
        }
        bufferStartOffset_ = lsn + encoded.size();
    } else {
        if (encoded.size() > bufferCapacity_ - buffer_.size()) writeBuffer();
        if (buffer_.empty()) bufferStartOffset_ = lsn;
        buffer_.insert(buffer_.end(), encoded.begin(), encoded.end());
    }

    nextLsn_ += encoded.size();
    lastAppendedLsn_ = lsn;
    knownLsns_.insert(lsn);
    ++stats_.recordsAppended;
    stats_.bytesAppended += encoded.size();
    return lsn;
}

bool LogManager::containsLsn(Lsn lsn) const noexcept {
    return segmented_ != nullptr
        ? (knownLsns_.contains(lsn) || segmented_->containsLsn(lsn))
        : isValidLsn(lsn) && knownLsns_.contains(lsn);
}

void LogManager::flushUpTo(Lsn target) {
    ++stats_.flushUpToCalls;
    if (!containsLsn(target)) {
        throw WalError(WalErrorKind::InvalidArgument, "flushUpTo target is not an appended LSN");
    }
    if (isValidLsn(durableLsn_) && durableLsn_ >= target) return;
    if (segmented_ != nullptr) {
        writeBuffer();
        const auto before = segmented_->stats();
        segmented_->flush();
        const auto after = segmented_->stats();
        stats_.fsyncCalls += after.fsyncCalls - before.fsyncCalls;
    } else {
        writeBuffer();
        syncWal();
    }
    durableLsn_ = lastAppendedLsn_;
}

void LogManager::flushAll() {
    if (!isValidLsn(lastAppendedLsn_)) return;
    flushUpTo(lastAppendedLsn_);
}

WalScanResult LogManager::scanIncludingBuffer() const {
    auto result = segmented_ != nullptr ? segmented_->scan() : scanWalFile(path_);
    if (result.truncatedTail || buffer_.empty()) return result;
    if (result.validBytes != bufferStartOffset_ || result.fileBytes != bufferStartOffset_) {
        throw std::logic_error("WAL disk and in-memory buffer offsets disagree");
    }
    std::size_t offset = 0;
    while (offset < buffer_.size()) {
        if (buffer_.size() - offset < wal_record_layout::HEADER_SIZE) {
            throw std::logic_error("LogManager buffer contains a partial record header");
        }
        const auto totalLength = byte_codec::readUint32(
            std::span<const std::byte>(buffer_).subspan(offset),
            wal_record_layout::TOTAL_LENGTH_OFFSET);
        if (totalLength > buffer_.size() - offset) {
            throw std::logic_error("LogManager buffer contains a partial record");
        }
        const auto lsn = static_cast<Lsn>(bufferStartOffset_ + offset);
        result.records.push_back(decodeWalRecord(
            std::span<const std::byte>(buffer_).subspan(offset, totalLength), lsn));
        offset += totalLength;
    }
    result.validBytes += buffer_.size();
    result.fileBytes += buffer_.size();
    return result;
}

WalScanResult LogManager::scan() const {
    return scanIncludingBuffer();
}

WalScanResult LogManager::scanFrom(WalOffset startOffset) const {
    if (!buffer_.empty()) {
        const auto complete = scanIncludingBuffer();
        if (startOffset < wal_file_layout::HEADER_SIZE || startOffset > complete.validBytes) {
            throw WalError(WalErrorKind::InvalidArgument, "WAL scan offset is outside the log");
        }
        WalScanResult result = complete;
        result.startOffset = startOffset;
        result.records.erase(
            result.records.begin(),
            std::lower_bound(result.records.begin(), result.records.end(), startOffset,
                             [](const LogRecord& record, WalOffset offset) {
                                 return record.lsn < offset;
                             }));
        if (!result.records.empty() && result.records.front().lsn != startOffset
            && startOffset != result.validBytes) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "WAL scan offset is not a record boundary");
        }
        return result;
    }
    if (segmented_ != nullptr) return segmented_->scanFrom(startOffset);
    return scanWalFileFrom(path_, startOffset);
}

LogRecord LogManager::readRecordAt(Lsn lsn) const {
    if (!buffer_.empty() && lsn >= bufferStartOffset_) {
        const auto offset = static_cast<std::size_t>(lsn - bufferStartOffset_);
        if (offset >= buffer_.size()
            || buffer_.size() - offset < wal_record_layout::HEADER_SIZE) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "Referenced buffered WAL record does not exist");
        }
        const auto totalLength = byte_codec::readUint32(
            std::span<const std::byte>(buffer_).subspan(offset),
            wal_record_layout::TOTAL_LENGTH_OFFSET);
        if (totalLength > buffer_.size() - offset) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "Referenced buffered WAL record is incomplete");
        }
        return decodeWalRecord(
            std::span<const std::byte>(buffer_).subspan(offset, totalLength), lsn);
    }
    if (segmented_ != nullptr) return segmented_->readRecordAt(lsn);
    return readWalRecordAt(path_, lsn);
}

std::uint64_t LogManager::physicalFileSize() const {
    if (segmented_ != nullptr) return segmented_->physicalBytes();
    return descriptorSize(descriptor_);
}

void LogManager::completeRecoveryScan(const WalScanResult& scan, Lsn baseDurableLsn) {
    if (!recoveryPending_) return;
    if (scan.truncatedTail) {
        nextLsn_ = scan.validBytes;
        truncatedTail_ = true;
        truncateToLastValidRecord();
    } else {
        nextLsn_ = scan.validBytes;
    }
    bufferStartOffset_ = nextLsn_;
    knownLsns_.clear();
    const auto knownScan = segmented_ != nullptr ? segmented_->scan() : scan;
    for (const auto& record : knownScan.records) knownLsns_.insert(record.lsn);
    if (isValidLsn(baseDurableLsn)) knownLsns_.insert(baseDurableLsn);
    lastAppendedLsn_ = scan.records.empty() ? baseDurableLsn : scan.records.back().lsn;
    durableLsn_ = lastAppendedLsn_;
    recoveryPending_ = false;
}

void LogManager::validate() const {
    const auto result = scanIncludingBuffer();
    if (result.truncatedTail != truncatedTail_) {
        throw std::logic_error("LogManager truncated-tail state disagrees with scanner");
    }
    if (!result.truncatedTail && result.validBytes != nextLsn_) {
        throw std::logic_error("LogManager next LSN disagrees with scanned WAL end");
    }
    if (openMode_ != LogOpenMode::DeferredRecovery
        && result.records.size() != knownLsns_.size()) {
        throw std::logic_error("LogManager known-LSN set disagrees with scanned records");
    }
    for (const auto& record : result.records) {
        if (!knownLsns_.contains(record.lsn)) {
            throw std::logic_error("Scanned WAL record is absent from known-LSN set");
        }
    }
    const auto scannedLast = result.records.empty() ? INVALID_LSN : result.records.back().lsn;
    if (scannedLast != lastAppendedLsn_) {
        throw std::logic_error("LogManager last-appended LSN disagrees with scanner");
    }
    if (isValidLsn(durableLsn_) && !knownLsns_.contains(durableLsn_)) {
        throw std::logic_error("LogManager durable LSN is not a known record");
    }
}

void LogManager::truncateToLastValidRecord() {
    if (!truncatedTail_) return;
    if (segmented_ != nullptr) {
        segmented_->truncateActiveTail();
        truncatedTail_ = false;
        bufferStartOffset_ = nextLsn_;
        return;
    }
    if (::ftruncate(descriptor_, static_cast<off_t>(nextLsn_)) != 0) {
        throwIo("Could not truncate incomplete WAL tail");
    }
    syncWal();
    truncatedTail_ = false;
    bufferStartOffset_ = nextLsn_;
}

LogManagerStats LogManager::stats() const noexcept {
    auto result = stats_;
    result.bufferedBytes = buffer_.size();
    result.lastAppendedLsn = lastAppendedLsn_;
    result.durableLsn = durableLsn_;
    result.logicalWalEnd = nextLsn_;
    if (segmented_ != nullptr) {
        const auto storage = segmented_->stats();
        result.segmentsCreated = storage.segmentsCreated;
        result.segmentsClosed = storage.segmentsClosed;
        result.segmentsDeleted = storage.segmentsDeleted;
        result.segmentRotations = storage.segmentRotations;
        result.segmentDirectorySyncs = storage.segmentDirectorySyncs;
        result.retainedSegments = storage.retainedSegments;
        result.activeSegmentId = storage.activeSegmentId;
        result.oldestRetainedLsn = storage.oldestRetainedLsn;
        result.walBytesReclaimed = storage.walBytesReclaimed;
        result.physicalWalBytes = storage.physicalWalBytes;
    } else if (descriptor_ >= 0) {
        struct stat status {};
        if (::fstat(descriptor_, &status) == 0 && status.st_size >= 0) {
            result.physicalWalBytes = static_cast<std::uint64_t>(status.st_size);
        }
    }
    return result;
}

void LogManager::resetStats() noexcept {
    stats_ = {};
    if (segmented_ != nullptr) segmented_->resetStats();
}

void LogManager::rotateSegment() {
    if (segmented_ == nullptr) return;
    const auto before = segmented_->stats();
    segmented_->rotate();
    const auto after = segmented_->stats();
    stats_.fsyncCalls += after.fsyncCalls - before.fsyncCalls;
    stats_.physicalWrites += after.physicalWrites - before.physicalWrites;
    stats_.bytesWritten += after.bytesWritten - before.bytesWritten;
}

std::uint64_t LogManager::reclaimSegmentsBefore(
    Lsn floorLsn, std::size_t extraSegments) {
    if (segmented_ == nullptr) return 0;
    const auto reclaimed = segmented_->reclaimBefore(floorLsn, extraSegments);
    knownLsns_.clear();
    for (const auto& record : segmented_->scan().records) knownLsns_.insert(record.lsn);
    return reclaimed;
}

void LogManager::migrateLegacyToSegmented() {
    if (!legacyMigrationPending()) return;
    flushAll();
    migrateLegacyWalToSegments(path_, segmentedPath_, segmentPayloadCapacity_);
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    segmented_ = std::make_unique<SegmentedWalStorage>(
        segmentedPath_, segmentPayloadCapacity_, false);
    activeStorageMode_ = WalStorageMode::Segmented;
    const auto scan = segmented_->scan();
    nextLsn_ = scan.validBytes;
    bufferStartOffset_ = nextLsn_;
    knownLsns_.clear();
    for (const auto& record : scan.records) knownLsns_.insert(record.lsn);
    lastAppendedLsn_ = scan.records.empty() ? INVALID_LSN : scan.records.back().lsn;
    durableLsn_ = lastAppendedLsn_;
    recoveryFailPoint("wal_migration_before_legacy_delete");
    if (::unlink(path_.c_str()) != 0 && errno != ENOENT) {
        throwIo("Could not delete obsolete legacy WAL");
    }
    recoveryFailPoint("wal_migration_after_legacy_delete");
    auto parent = std::filesystem::path(path_).parent_path();
    if (parent.empty()) parent = ".";
    syncDirectory(parent.string());
    recoveryFailPoint("wal_migration_after_legacy_parent_sync");
}

} // namespace minidb
