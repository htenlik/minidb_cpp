#pragma once

#include "minidb/segmented_wal.hpp"
#include "minidb/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace minidb {

enum class LogOpenMode {
    EagerValidated,
    DeferredRecovery,
};

enum class WalStorageMode {
    LegacySingleFile,
    Segmented,
    Auto,
};

struct LogManagerStats {
    std::uint64_t recordsAppended = 0;
    std::uint64_t bytesAppended = 0;
    std::uint64_t bufferFlushes = 0;
    std::uint64_t physicalWrites = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t fsyncCalls = 0;
    std::uint64_t flushUpToCalls = 0;
    std::uint64_t bufferedBytes = 0;
    std::uint64_t segmentsCreated = 0;
    std::uint64_t segmentsClosed = 0;
    std::uint64_t segmentsDeleted = 0;
    std::uint64_t segmentRotations = 0;
    std::uint64_t segmentDirectorySyncs = 0;
    std::uint64_t retainedSegments = 0;
    std::uint64_t physicalWalBytes = 0;
    std::uint64_t walBytesReclaimed = 0;
    WalSegmentId activeSegmentId = INVALID_WAL_SEGMENT_ID;
    Lsn oldestRetainedLsn = INVALID_LSN;
    Lsn logicalWalEnd = wal_file_layout::HEADER_SIZE;
    Lsn lastAppendedLsn = INVALID_LSN;
    Lsn durableLsn = INVALID_LSN;

    bool operator==(const LogManagerStats&) const = default;
};

class LogManager final : public WalFlushProvider {
public:
    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 64 * 1024;

    explicit LogManager(
        std::string walPath,
        std::size_t bufferCapacity = DEFAULT_BUFFER_SIZE,
        LogOpenMode openMode = LogOpenMode::EagerValidated,
        WalStorageMode storageMode = WalStorageMode::LegacySingleFile,
        std::uint32_t segmentPayloadCapacity = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY);
    ~LogManager() override;

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    [[nodiscard]] Lsn append(LogRecord record);
    void flushUpTo(Lsn target) override;
    void flushAll();

    [[nodiscard]] Lsn durableLsn() const noexcept override { return durableLsn_; }
    [[nodiscard]] Lsn lastAppendedLsn() const noexcept { return lastAppendedLsn_; }
    [[nodiscard]] bool containsLsn(Lsn lsn) const noexcept override;
    [[nodiscard]] bool hasTruncatedTail() const noexcept { return truncatedTail_; }
    [[nodiscard]] std::uint64_t lastValidOffset() const noexcept { return nextLsn_; }
    [[nodiscard]] std::uint64_t physicalFileSize() const;
    [[nodiscard]] std::uint64_t physicalWalBytes() const { return physicalFileSize(); }
    [[nodiscard]] Lsn oldestRetainedLsn() const noexcept {
        return segmented_ != nullptr && !segmented_->segments().empty()
            ? segmented_->segments().front().header.startLsn
            : wal_file_layout::HEADER_SIZE;
    }
    [[nodiscard]] bool isSegmented() const noexcept {
        return activeStorageMode_ == WalStorageMode::Segmented;
    }
    [[nodiscard]] bool legacyMigrationPending() const noexcept {
        return activeStorageMode_ == WalStorageMode::LegacySingleFile
            && requestedStorageMode_ == WalStorageMode::Auto;
    }
    [[nodiscard]] const std::string& segmentedPath() const noexcept { return segmentedPath_; }
    [[nodiscard]] bool recoveryPending() const noexcept { return recoveryPending_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] WalScanResult scan() const;
    [[nodiscard]] WalScanResult scanFrom(WalOffset startOffset) const;
    [[nodiscard]] LogRecord readRecordAt(Lsn lsn) const;
    void validate() const;
    void truncateToLastValidRecord();
    void completeRecoveryScan(const WalScanResult& scan, Lsn baseDurableLsn = INVALID_LSN);
    void rotateSegment();
    [[nodiscard]] std::uint64_t reclaimSegmentsBefore(
        Lsn floorLsn, std::size_t extraSegments = 0);
    void migrateLegacyToSegmented();

    [[nodiscard]] LogManagerStats stats() const noexcept;
    void resetStats() noexcept;

private:
    std::string path_;
    std::string segmentedPath_;
    int descriptor_ = -1;
    std::size_t bufferCapacity_;
    std::vector<std::byte> buffer_;
    std::uint64_t bufferStartOffset_ = wal_file_layout::HEADER_SIZE;
    std::uint64_t nextLsn_ = wal_file_layout::HEADER_SIZE;
    Lsn lastAppendedLsn_ = INVALID_LSN;
    Lsn durableLsn_ = INVALID_LSN;
    bool truncatedTail_ = false;
    bool recoveryPending_ = false;
    LogOpenMode openMode_ = LogOpenMode::EagerValidated;
    WalStorageMode requestedStorageMode_ = WalStorageMode::LegacySingleFile;
    WalStorageMode activeStorageMode_ = WalStorageMode::LegacySingleFile;
    std::uint32_t segmentPayloadCapacity_ = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY;
    std::unique_ptr<SegmentedWalStorage> segmented_;
    std::unordered_set<Lsn> knownLsns_;
    LogManagerStats stats_{};

    void openOrCreate();
    void openSegmented();
    void initializeNewWal();
    void loadExistingWal();
    void loadExistingWalDeferred();
    void writeBuffer();
    void writeBytes(std::uint64_t offset, std::span<const std::byte> bytes);
    void syncWal();
    [[nodiscard]] WalScanResult scanIncludingBuffer() const;
};

} // namespace minidb
