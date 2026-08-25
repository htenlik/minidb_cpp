#pragma once

#include "minidb/wal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {

namespace wal_segment_layout {
inline constexpr std::array<std::byte, 8> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'W'},
    std::byte{'S'}, std::byte{'E'}, std::byte{'G'}, std::byte{'1'},
};
inline constexpr std::uint32_t CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t SEGMENT_ID_OFFSET = 16;
inline constexpr std::size_t START_LSN_OFFSET = 24;
inline constexpr std::size_t PREVIOUS_SEGMENT_ID_OFFSET = 32;
inline constexpr std::size_t PREVIOUS_LOGICAL_END_OFFSET = 40;
inline constexpr std::size_t PAYLOAD_CAPACITY_OFFSET = 48;
inline constexpr std::size_t FLAGS_OFFSET = 52;
inline constexpr std::size_t CHECKSUM_OFFSET = 56;
inline constexpr std::size_t RESERVED_OFFSET = 60;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::uint32_t DEFAULT_PAYLOAD_CAPACITY = 16U * 1024U * 1024U;
}

namespace wal_manifest_layout {
inline constexpr std::array<std::byte, 8> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'W'},
    std::byte{'S'}, std::byte{'M'}, std::byte{'0'}, std::byte{'1'},
};
inline constexpr std::uint32_t CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t PAYLOAD_CAPACITY_OFFSET = 16;
inline constexpr std::size_t FLAGS_OFFSET = 20;
inline constexpr std::size_t FIRST_RETAINED_SEGMENT_ID_OFFSET = 24;
inline constexpr std::size_t INITIAL_LSN_OFFSET = 32;
inline constexpr std::size_t MIGRATION_BASE_LSN_OFFSET = 40;
inline constexpr std::size_t RESERVED64_OFFSET = 48;
inline constexpr std::size_t CHECKSUM_OFFSET = 56;
inline constexpr std::size_t RESERVED_OFFSET = 60;
inline constexpr std::size_t HEADER_SIZE = 64;
}

struct WalSegmentHeader {
    WalSegmentId segmentId = INVALID_WAL_SEGMENT_ID;
    Lsn startLsn = INVALID_LSN;
    WalSegmentId previousSegmentId = INVALID_WAL_SEGMENT_ID;
    Lsn previousLogicalEndLsn = INVALID_LSN;
    std::uint32_t payloadCapacity = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY;
    std::uint32_t flags = 0;
    bool operator==(const WalSegmentHeader&) const = default;
};

struct WalSegmentManifest {
    std::uint32_t payloadCapacity = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY;
    WalSegmentId firstRetainedSegmentId = 1;
    Lsn initialLsn = wal_file_layout::HEADER_SIZE;
    Lsn migrationBaseLsn = INVALID_LSN;
    bool operator==(const WalSegmentManifest&) const = default;
};

struct WalSegmentInfo {
    WalSegmentHeader header;
    std::string path;
    Lsn logicalEndLsn = INVALID_LSN;
    std::uint64_t physicalBytes = 0;
    bool active = false;
};

struct SegmentedWalStats {
    std::uint64_t segmentsCreated = 0;
    std::uint64_t segmentsClosed = 0;
    std::uint64_t segmentsDeleted = 0;
    std::uint64_t segmentRotations = 0;
    std::uint64_t segmentDirectorySyncs = 0;
    std::uint64_t physicalWrites = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t fsyncCalls = 0;
    std::uint64_t physicalWalBytes = 0;
    std::uint64_t walBytesReclaimed = 0;
    std::uint64_t retainedSegments = 0;
    WalSegmentId activeSegmentId = INVALID_WAL_SEGMENT_ID;
    Lsn oldestRetainedLsn = INVALID_LSN;
    Lsn logicalWalEnd = wal_file_layout::HEADER_SIZE;
};

[[nodiscard]] std::string segmentedWalPathForLegacyWal(std::string_view legacyWalPath);
[[nodiscard]] std::string segmentedWalPathForDatabase(std::string_view databasePath);
[[nodiscard]] std::string walSegmentFilename(WalSegmentId segmentId);

[[nodiscard]] std::array<std::byte, wal_segment_layout::HEADER_SIZE>
encodeWalSegmentHeader(const WalSegmentHeader& header);
[[nodiscard]] WalSegmentHeader decodeWalSegmentHeader(std::span<const std::byte> bytes);
[[nodiscard]] std::array<std::byte, wal_manifest_layout::HEADER_SIZE>
encodeWalSegmentManifest(const WalSegmentManifest& manifest);
[[nodiscard]] WalSegmentManifest decodeWalSegmentManifest(std::span<const std::byte> bytes);

// Sync directory-entry changes on supported POSIX filesystems.
void syncDirectory(const std::string& path);

class SegmentedWalStorage {
public:
    SegmentedWalStorage(
        std::string directory,
        std::uint32_t payloadCapacity = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY,
        bool createIfMissing = true);
    ~SegmentedWalStorage();

    SegmentedWalStorage(const SegmentedWalStorage&) = delete;
    SegmentedWalStorage& operator=(const SegmentedWalStorage&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return directory_; }
    [[nodiscard]] std::uint32_t payloadCapacity() const noexcept { return manifest_.payloadCapacity; }
    [[nodiscard]] Lsn logicalEnd() const noexcept { return logicalEnd_; }
    [[nodiscard]] Lsn lastRecordLsn() const noexcept { return lastRecordLsn_; }
    [[nodiscard]] Lsn durableLsn() const noexcept { return durableLsn_; }
    [[nodiscard]] bool truncatedTail() const noexcept { return truncatedTail_; }
    [[nodiscard]] const std::vector<WalSegmentInfo>& segments() const noexcept { return segments_; }

    void appendRecord(Lsn lsn, std::span<const std::byte> encoded);
    void flush();
    void rotate();
    void truncateActiveTail();
    [[nodiscard]] WalScanResult scan() const;
    [[nodiscard]] WalScanResult scanFrom(Lsn startLsn) const;
    [[nodiscard]] LogRecord readRecordAt(Lsn lsn) const;
    [[nodiscard]] bool containsLsn(Lsn lsn) const noexcept;
    [[nodiscard]] std::uint64_t physicalBytes() const;
    [[nodiscard]] SegmentedWalStats stats() const noexcept;
    void resetStats() noexcept { stats_ = {}; }

    // Retains the segment containing floorLsn and every later segment. Returns
    // the number of bytes unlinked. extraSegments conservatively retains that
    // many additional closed predecessors.
    [[nodiscard]] std::uint64_t reclaimBefore(
        Lsn floorLsn, std::size_t extraSegments = 0);

private:
    std::string directory_;
    WalSegmentManifest manifest_{};
    std::vector<WalSegmentInfo> segments_;
    int activeDescriptor_ = -1;
    Lsn logicalEnd_ = wal_file_layout::HEADER_SIZE;
    Lsn lastRecordLsn_ = INVALID_LSN;
    Lsn durableLsn_ = INVALID_LSN;
    bool truncatedTail_ = false;
    SegmentedWalStats stats_{};

    void createNewStore(std::uint32_t payloadCapacity);
    void openExistingStore();
    void discoverSegments();
    void openActiveDescriptor();
    void createSegment(Lsn startLsn);
    void publishManifest();
    [[nodiscard]] WalScanResult scanImpl(Lsn startLsn) const;
};

// Converts a complete legacy v1 stream into a temporary segmented directory,
// fsyncs it, atomically publishes it, and leaves the source untouched.
void migrateLegacyWalToSegments(
    const std::string& legacyWalPath,
    const std::string& segmentedDirectory,
    std::uint32_t payloadCapacity = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY);

} // namespace minidb
