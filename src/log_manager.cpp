#include "minidb/log_manager.hpp"

#include "minidb/byte_codec.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
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

LogManager::LogManager(std::string walPath, std::size_t bufferCapacity)
    : path_(std::move(walPath)), bufferCapacity_(bufferCapacity) {
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
    if (descriptor_ < 0) return;
    try {
        flushAll();
    } catch (...) {
    }
    ::close(descriptor_);
}

void LogManager::openOrCreate() {
    descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (descriptor_ < 0) throwIo("Could not open WAL file");
    try {
        if (descriptorSize(descriptor_) == 0) initializeNewWal();
        else loadExistingWal();
    } catch (...) {
        ::close(descriptor_);
        descriptor_ = -1;
        throw;
    }
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
    writeBytes(bufferStartOffset_, buffer_);
    bufferStartOffset_ += buffer_.size();
    buffer_.clear();
    ++stats_.bufferFlushes;
}

Lsn LogManager::append(LogRecord record) {
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
        writeBytes(lsn, encoded);
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
    return isValidLsn(lsn) && knownLsns_.contains(lsn);
}

void LogManager::flushUpTo(Lsn target) {
    ++stats_.flushUpToCalls;
    if (!containsLsn(target)) {
        throw WalError(WalErrorKind::InvalidArgument, "flushUpTo target is not an appended LSN");
    }
    if (isValidLsn(durableLsn_) && durableLsn_ >= target) return;
    writeBuffer();
    syncWal();
    durableLsn_ = lastAppendedLsn_;
}

void LogManager::flushAll() {
    if (!isValidLsn(lastAppendedLsn_)) return;
    flushUpTo(lastAppendedLsn_);
}

WalScanResult LogManager::scanIncludingBuffer() const {
    auto result = scanWalFile(path_);
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

void LogManager::validate() const {
    const auto result = scanIncludingBuffer();
    if (result.truncatedTail != truncatedTail_) {
        throw std::logic_error("LogManager truncated-tail state disagrees with scanner");
    }
    if (!result.truncatedTail && result.validBytes != nextLsn_) {
        throw std::logic_error("LogManager next LSN disagrees with scanned WAL end");
    }
    if (result.records.size() != knownLsns_.size()) {
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
    return result;
}

} // namespace minidb
