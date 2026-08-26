#include "minidb/segmented_wal.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/recovery.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace minidb {
namespace {

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(other.value_) { other.value_ = -1; }
    [[nodiscard]] int get() const noexcept { return value_; }
private:
    int value_;
};

[[noreturn]] void throwIo(const std::string& operation) {
    throw WalError(WalErrorKind::Io, operation + ": " + std::strerror(errno));
}

std::uint64_t descriptorSize(int descriptor) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) throwIo("Could not inspect WAL segment");
    if (status.st_size < 0) throw WalError(WalErrorKind::Io, "WAL segment has negative size");
    return static_cast<std::uint64_t>(status.st_size);
}

void readExact(int descriptor, std::uint64_t offset, std::span<std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::pread(descriptor, bytes.data() + done, bytes.size() - done,
                                   static_cast<off_t>(offset + done));
        if (count < 0) { if (errno == EINTR) continue; throwIo("Could not read WAL segment"); }
        if (count == 0) throw WalError(WalErrorKind::Io, "Unexpected WAL segment EOF");
        done += static_cast<std::size_t>(count);
    }
}

void writeExact(int descriptor, std::uint64_t offset, std::span<const std::byte> bytes,
                SegmentedWalStats* stats = nullptr) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::pwrite(descriptor, bytes.data() + done, bytes.size() - done,
                                    static_cast<off_t>(offset + done));
        if (count < 0) { if (errno == EINTR) continue; throwIo("Could not write WAL segment"); }
        if (count == 0) throw WalError(WalErrorKind::Io, "WAL segment write made no progress");
        done += static_cast<std::size_t>(count);
        if (stats != nullptr) {
            ++stats->physicalWrites;
            stats->bytesWritten += static_cast<std::uint64_t>(count);
        }
    }
}

void syncFile(int descriptor, SegmentedWalStats* stats = nullptr) {
    if (::fsync(descriptor) != 0) {
        throw WalError(WalErrorKind::Durability,
                       "Could not fsync WAL segment: " + std::string(std::strerror(errno)));
    }
    if (stats != nullptr) ++stats->fsyncCalls;
}

std::uint32_t checksumWithZeroField(
    std::span<const std::byte> bytes, std::size_t checksumOffset) {
    std::array<std::byte, 64> copy{};
    if (bytes.size() != copy.size()) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL metadata header has wrong size");
    }
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    std::fill_n(copy.begin() + checksumOffset, 4, std::byte{0});
    return crc32c(copy);
}

std::filesystem::path manifestPath(const std::string& directory) {
    return std::filesystem::path(directory) / "manifest";
}

std::filesystem::path segmentPath(const std::string& directory, WalSegmentId id) {
    return std::filesystem::path(directory) / walSegmentFilename(id);
}

std::optional<WalSegmentId> parseSegmentFilename(std::string_view name) {
    if (name.size() != 20 || name.substr(16) != ".seg") return std::nullopt;
    WalSegmentId id = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + 16, id, 16);
    if (error != std::errc{} || end != name.data() + 16 || id == 0) return std::nullopt;
    return id;
}

struct SegmentScan {
    std::vector<LogRecord> records;
    Lsn validEnd = INVALID_LSN;
    Lsn physicalLogicalEnd = INVALID_LSN;
    bool truncatedTail = false;
    std::uint64_t physicalBytes = 0;
};

SegmentScan scanOneSegment(const WalSegmentInfo& info, bool allowTruncatedTail) {
    const auto descriptor = ::open(info.path.c_str(), O_RDONLY);
    if (descriptor < 0) throwIo("Could not open WAL segment for scanning");
    const Descriptor owned(descriptor);
    SegmentScan result;
    result.physicalBytes = descriptorSize(descriptor);
    if (result.physicalBytes < wal_segment_layout::HEADER_SIZE
        || result.physicalBytes > wal_segment_layout::HEADER_SIZE
            + info.header.payloadCapacity) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL segment file size is invalid");
    }
    std::array<std::byte, wal_segment_layout::HEADER_SIZE> headerBytes{};
    readExact(descriptor, 0, headerBytes);
    if (decodeWalSegmentHeader(headerBytes) != info.header) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL segment header changed during scan");
    }
    const auto payloadBytes = result.physicalBytes - wal_segment_layout::HEADER_SIZE;
    Lsn logical = info.header.startLsn;
    std::uint64_t local = wal_segment_layout::HEADER_SIZE;
    while (local < result.physicalBytes) {
        const auto remaining = result.physicalBytes - local;
        if (remaining < wal_record_layout::HEADER_SIZE) {
            result.truncatedTail = true;
            break;
        }
        std::array<std::byte, wal_record_layout::HEADER_SIZE> recordHeader{};
        readExact(descriptor, local, recordHeader);
        const auto total = byte_codec::readUint32(
            recordHeader, wal_record_layout::TOTAL_LENGTH_OFFSET);
        if (total < wal_record_layout::HEADER_SIZE
            || total > wal_record_layout::MAX_RECORD_SIZE) {
            // A record-looking corrupt prefix is not a repairable short write.
            static_cast<void>(decodeWalRecord(recordHeader, logical));
        }
        if (total > remaining) {
            result.truncatedTail = true;
            break;
        }
        std::vector<std::byte> encoded(total);
        readExact(descriptor, local, encoded);
        result.records.push_back(decodeWalRecord(encoded, logical));
        local += total;
        logical += total;
    }
    if (result.truncatedTail && !allowTruncatedTail) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "Closed WAL segment contains a truncated record");
    }
    result.validEnd = logical;
    result.physicalLogicalEnd = info.header.startLsn + payloadBytes;
    return result;
}

} // namespace

std::string segmentedWalPathForLegacyWal(std::string_view legacyWalPath) {
    return std::string(legacyWalPath) + ".d";
}

std::string segmentedWalPathForDatabase(std::string_view databasePath) {
    return std::string(databasePath) + ".wal.d";
}

std::string walSegmentFilename(WalSegmentId segmentId) {
    if (segmentId == INVALID_WAL_SEGMENT_ID) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL segment ID zero is invalid");
    }
    constexpr char HEX[] = "0123456789abcdef";
    std::string result(16, '0');
    auto value = segmentId;
    for (std::size_t index = result.size(); index-- > 0;) {
        result[index] = HEX[value & 0xFU];
        value >>= 4U;
    }
    return result + ".seg";
}

std::array<std::byte, wal_segment_layout::HEADER_SIZE>
encodeWalSegmentHeader(const WalSegmentHeader& header) {
    using namespace wal_segment_layout;
    if (header.segmentId == INVALID_WAL_SEGMENT_ID
        || !isValidLsn(header.startLsn) || header.startLsn < wal_file_layout::HEADER_SIZE
        || !isValidLsn(header.previousLogicalEndLsn)
        || header.previousLogicalEndLsn != header.startLsn
        || header.payloadCapacity < 128
        || header.flags != 0
        || (header.segmentId == 1) != (header.previousSegmentId == INVALID_WAL_SEGMENT_ID)) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid WAL segment header fields");
    }
    std::array<std::byte, HEADER_SIZE> bytes{};
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin());
    byte_codec::writeUint32(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(bytes, HEADER_SIZE_OFFSET, HEADER_SIZE);
    byte_codec::writeUint64(bytes, SEGMENT_ID_OFFSET, header.segmentId);
    byte_codec::writeUint64(bytes, START_LSN_OFFSET, header.startLsn);
    byte_codec::writeUint64(bytes, PREVIOUS_SEGMENT_ID_OFFSET, header.previousSegmentId);
    byte_codec::writeUint64(bytes, PREVIOUS_LOGICAL_END_OFFSET, header.previousLogicalEndLsn);
    byte_codec::writeUint32(bytes, PAYLOAD_CAPACITY_OFFSET, header.payloadCapacity);
    byte_codec::writeUint32(bytes, FLAGS_OFFSET, header.flags);
    byte_codec::writeUint32(bytes, CHECKSUM_OFFSET,
                            checksumWithZeroField(bytes, CHECKSUM_OFFSET));
    return bytes;
}

WalSegmentHeader decodeWalSegmentHeader(std::span<const std::byte> bytes) {
    using namespace wal_segment_layout;
    if (bytes.size() != HEADER_SIZE
        || !std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin())
        || byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION
        || byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint32(bytes, CHECKSUM_OFFSET)
            != checksumWithZeroField(bytes, CHECKSUM_OFFSET)
        || byte_codec::readUint32(bytes, RESERVED_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptHeader, "Invalid WAL segment header");
    }
    WalSegmentHeader header{
        byte_codec::readUint64(bytes, SEGMENT_ID_OFFSET),
        byte_codec::readUint64(bytes, START_LSN_OFFSET),
        byte_codec::readUint64(bytes, PREVIOUS_SEGMENT_ID_OFFSET),
        byte_codec::readUint64(bytes, PREVIOUS_LOGICAL_END_OFFSET),
        byte_codec::readUint32(bytes, PAYLOAD_CAPACITY_OFFSET),
        byte_codec::readUint32(bytes, FLAGS_OFFSET),
    };
    if (header.segmentId == INVALID_WAL_SEGMENT_ID
        || !isValidLsn(header.startLsn) || header.startLsn < wal_file_layout::HEADER_SIZE
        || header.previousLogicalEndLsn != header.startLsn
        || header.payloadCapacity < 128 || header.flags != 0
        || (header.segmentId == 1) != (header.previousSegmentId == INVALID_WAL_SEGMENT_ID)) {
        throw WalError(WalErrorKind::CorruptHeader, "Malformed WAL segment header fields");
    }
    return header;
}

std::array<std::byte, wal_manifest_layout::HEADER_SIZE>
encodeWalSegmentManifest(const WalSegmentManifest& manifest) {
    using namespace wal_manifest_layout;
    if (manifest.payloadCapacity < 128
        || manifest.firstRetainedSegmentId == INVALID_WAL_SEGMENT_ID
        || !isValidLsn(manifest.initialLsn)
        || manifest.initialLsn < wal_file_layout::HEADER_SIZE
        || (manifest.migrationBaseLsn != INVALID_LSN
            && manifest.migrationBaseLsn != manifest.initialLsn)) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid segmented-WAL manifest fields");
    }
    std::array<std::byte, HEADER_SIZE> bytes{};
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin());
    byte_codec::writeUint32(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(bytes, HEADER_SIZE_OFFSET, HEADER_SIZE);
    byte_codec::writeUint32(bytes, PAYLOAD_CAPACITY_OFFSET, manifest.payloadCapacity);
    byte_codec::writeUint64(bytes, FIRST_RETAINED_SEGMENT_ID_OFFSET,
                            manifest.firstRetainedSegmentId);
    byte_codec::writeUint64(bytes, INITIAL_LSN_OFFSET, manifest.initialLsn);
    byte_codec::writeUint64(bytes, MIGRATION_BASE_LSN_OFFSET, manifest.migrationBaseLsn);
    byte_codec::writeUint32(bytes, CHECKSUM_OFFSET,
                            checksumWithZeroField(bytes, CHECKSUM_OFFSET));
    return bytes;
}

WalSegmentManifest decodeWalSegmentManifest(std::span<const std::byte> bytes) {
    using namespace wal_manifest_layout;
    if (bytes.size() != HEADER_SIZE
        || !std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin())
        || byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION
        || byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint32(bytes, FLAGS_OFFSET) != 0
        || byte_codec::readUint64(bytes, RESERVED64_OFFSET) != 0
        || byte_codec::readUint32(bytes, RESERVED_OFFSET) != 0
        || byte_codec::readUint32(bytes, CHECKSUM_OFFSET)
            != checksumWithZeroField(bytes, CHECKSUM_OFFSET)) {
        throw WalError(WalErrorKind::CorruptHeader, "Invalid segmented-WAL manifest");
    }
    WalSegmentManifest manifest{
        byte_codec::readUint32(bytes, PAYLOAD_CAPACITY_OFFSET),
        byte_codec::readUint64(bytes, FIRST_RETAINED_SEGMENT_ID_OFFSET),
        byte_codec::readUint64(bytes, INITIAL_LSN_OFFSET),
        byte_codec::readUint64(bytes, MIGRATION_BASE_LSN_OFFSET),
    };
    if (manifest.payloadCapacity < 128
        || manifest.firstRetainedSegmentId == INVALID_WAL_SEGMENT_ID
        || !isValidLsn(manifest.initialLsn)
        || manifest.initialLsn < wal_file_layout::HEADER_SIZE
        || (manifest.migrationBaseLsn != INVALID_LSN
            && manifest.migrationBaseLsn != manifest.initialLsn)) {
        throw WalError(WalErrorKind::CorruptHeader, "Malformed segmented-WAL manifest fields");
    }
    return manifest;
}

void syncDirectory(const std::string& path) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throwIo("Could not open directory for durability sync");
    const Descriptor owned(descriptor);
    if (::fsync(descriptor) != 0) {
        throw WalError(WalErrorKind::Durability,
                       "Could not fsync directory: " + std::string(std::strerror(errno)));
    }
}

SegmentedWalStorage::SegmentedWalStorage(
    std::string directory,
    std::uint32_t payloadCapacity,
    bool createIfMissing,
    Lsn migrationBaseLsn)
    : directory_(std::move(directory)) {
    if (directory_.empty() || payloadCapacity < 128) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid segmented-WAL configuration");
    }
    if (std::filesystem::exists(directory_)) {
        openExistingStore();
        if (manifest_.payloadCapacity != payloadCapacity) {
            throw WalError(WalErrorKind::InvalidArgument,
                           "Configured WAL segment capacity disagrees with manifest");
        }
    }
    else if (createIfMissing) createNewStore(payloadCapacity, migrationBaseLsn);
    else throw WalError(WalErrorKind::Io, "Segmented WAL directory does not exist");
}

SegmentedWalStorage::~SegmentedWalStorage() {
    if (activeDescriptor_ < 0) return;
    try { flush(); } catch (...) {}
    ::close(activeDescriptor_);
}

void SegmentedWalStorage::createNewStore(
    std::uint32_t payloadCapacity, Lsn migrationBaseLsn) {
    std::error_code error;
    if (!std::filesystem::create_directory(directory_, error) || error) {
        throw WalError(WalErrorKind::Io, "Could not create segmented WAL directory: "
            + error.message());
    }
    manifest_.payloadCapacity = payloadCapacity;
    manifest_.firstRetainedSegmentId = 1;
    manifest_.initialLsn = migrationBaseLsn == INVALID_LSN
        ? wal_file_layout::HEADER_SIZE : migrationBaseLsn;
    manifest_.migrationBaseLsn = migrationBaseLsn;
    publishManifest();
    createSegment(manifest_.initialLsn);
    syncDirectory(std::filesystem::path(directory_).parent_path().string());
    ++stats_.segmentDirectorySyncs;
}

void SegmentedWalStorage::openExistingStore() {
    const auto descriptor = ::open(manifestPath(directory_).c_str(), O_RDONLY);
    if (descriptor < 0) throwIo("Could not open segmented-WAL manifest");
    const Descriptor owned(descriptor);
    if (descriptorSize(descriptor) != wal_manifest_layout::HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptHeader, "Segmented-WAL manifest size is invalid");
    }
    std::array<std::byte, wal_manifest_layout::HEADER_SIZE> bytes{};
    readExact(descriptor, 0, bytes);
    manifest_ = decodeWalSegmentManifest(bytes);
    discoverSegments();
    logicalEnd_ = segments_.back().logicalEndLsn;
    openActiveDescriptor();
    const auto scanned = scan();
    logicalEnd_ = scanned.validBytes;
    truncatedTail_ = scanned.truncatedTail;
    lastRecordLsn_ = scanned.records.empty() ? INVALID_LSN : scanned.records.back().lsn;
    durableLsn_ = lastRecordLsn_;
}

void SegmentedWalStorage::discoverSegments() {
    std::vector<std::pair<WalSegmentId, std::string>> found;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name == "manifest" || name == "manifest.tmp") continue;
        const auto id = parseSegmentFilename(name);
        if (!id.has_value()) {
            if (name.size() == 20 && name.ends_with(".seg")) {
                throw WalError(WalErrorKind::CorruptHeader,
                               "Malformed WAL segment filename");
            }
            continue;
        }
        if (*id < manifest_.firstRetainedSegmentId) continue; // safely published extra history
        found.emplace_back(*id, entry.path().string());
    }
    std::sort(found.begin(), found.end());
    if (found.empty() || found.front().first != manifest_.firstRetainedSegmentId) {
        throw WalError(WalErrorKind::CorruptHeader, "Required first retained WAL segment is missing");
    }
    segments_.clear();
    for (std::size_t index = 0; index < found.size(); ++index) {
        if (index != 0 && found[index].first <= found[index - 1].first) {
            throw WalError(WalErrorKind::CorruptHeader, "Duplicate/backwards WAL segment IDs");
        }
        const auto descriptor = ::open(found[index].second.c_str(), O_RDONLY);
        if (descriptor < 0) throwIo("Could not open WAL segment header");
        const Descriptor owned(descriptor);
        std::array<std::byte, wal_segment_layout::HEADER_SIZE> bytes{};
        readExact(descriptor, 0, bytes);
        const auto header = decodeWalSegmentHeader(bytes);
        if (header.segmentId != found[index].first
            || header.payloadCapacity != manifest_.payloadCapacity) {
            throw WalError(WalErrorKind::CorruptHeader,
                           "WAL segment filename/header identity mismatch");
        }
        WalSegmentInfo info{header, found[index].second, INVALID_LSN,
                            descriptorSize(descriptor), index + 1 == found.size()};
        const auto scanned = scanOneSegment(info, info.active);
        info.logicalEndLsn = scanned.validEnd;
        if (index != 0) {
            const auto& previous = segments_.back();
            if (header.segmentId <= previous.header.segmentId
                || header.previousSegmentId != previous.header.segmentId
                || header.startLsn != previous.logicalEndLsn
                || header.previousLogicalEndLsn != previous.logicalEndLsn) {
                throw WalError(WalErrorKind::CorruptHeader, "WAL segment chain is discontinuous");
            }
        }
        segments_.push_back(std::move(info));
    }
}

void SegmentedWalStorage::openActiveDescriptor() {
    if (segments_.empty()) throw std::logic_error("Segmented WAL has no active segment");
    activeDescriptor_ = ::open(segments_.back().path.c_str(), O_RDWR);
    if (activeDescriptor_ < 0) throwIo("Could not open active WAL segment");
}

void SegmentedWalStorage::createSegment(Lsn startLsn) {
    if (!segments_.empty()) {
        flush();
        ::close(activeDescriptor_);
        activeDescriptor_ = -1;
        segments_.back().active = false;
        ++stats_.segmentsClosed;
    }
    const auto id = segments_.empty() ? WalSegmentId{1} : segments_.back().header.segmentId + 1;
    if (id == INVALID_WAL_SEGMENT_ID) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL segment ID space is exhausted");
    }
    const auto previousId = segments_.empty()
        ? INVALID_WAL_SEGMENT_ID : segments_.back().header.segmentId;
    WalSegmentHeader header{id, startLsn, previousId, startLsn,
                            manifest_.payloadCapacity, 0};
    const auto path = segmentPath(directory_, id).string();
    const auto descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) throwIo("Could not create WAL segment");
    activeDescriptor_ = descriptor;
    try {
        const auto bytes = encodeWalSegmentHeader(header);
        writeExact(descriptor, 0, bytes, &stats_);
        syncFile(descriptor, &stats_);
        syncDirectory(directory_);
        ++stats_.segmentDirectorySyncs;
    } catch (...) {
        ::close(activeDescriptor_);
        activeDescriptor_ = -1;
        throw;
    }
    segments_.push_back(WalSegmentInfo{header, path, startLsn,
                                       wal_segment_layout::HEADER_SIZE, true});
    logicalEnd_ = startLsn;
    truncatedTail_ = false;
    ++stats_.segmentsCreated;
}

void SegmentedWalStorage::publishManifest() {
    const auto bytes = encodeWalSegmentManifest(manifest_);
    const auto temporary = std::filesystem::path(directory_) / "manifest.tmp";
    const auto final = manifestPath(directory_);
    const auto descriptor = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) throwIo("Could not create segmented-WAL manifest temporary");
    const Descriptor owned(descriptor);
    writeExact(descriptor, 0, bytes, &stats_);
    syncFile(descriptor, &stats_);
    if (::rename(temporary.c_str(), final.c_str()) != 0) throwIo("Could not publish WAL manifest");
    syncDirectory(directory_);
    ++stats_.segmentDirectorySyncs;
}

void SegmentedWalStorage::appendRecord(Lsn lsn, std::span<const std::byte> encoded) {
    if (truncatedTail_) {
        throw WalError(WalErrorKind::TruncatedTail,
                       "Cannot append before truncating active WAL tail");
    }
    if (lsn != logicalEnd_ || encoded.empty()
        || encoded.size() > manifest_.payloadCapacity) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid segmented WAL append boundary");
    }
    auto& active = segments_.back();
    const auto used = logicalEnd_ - active.header.startLsn;
    if (encoded.size() > manifest_.payloadCapacity - used) {
        rotate();
    }
    auto& destination = segments_.back();
    const auto local = wal_segment_layout::HEADER_SIZE
        + (lsn - destination.header.startLsn);
    writeExact(activeDescriptor_, local, encoded, &stats_);
    logicalEnd_ += encoded.size();
    destination.logicalEndLsn = logicalEnd_;
    destination.physicalBytes = local + encoded.size();
    lastRecordLsn_ = lsn;
}

void SegmentedWalStorage::flush() {
    if (activeDescriptor_ < 0) return;
    syncFile(activeDescriptor_, &stats_);
    durableLsn_ = lastRecordLsn_;
}

void SegmentedWalStorage::rotate() {
    createSegment(logicalEnd_);
    ++stats_.segmentRotations;
}

void SegmentedWalStorage::truncateActiveTail() {
    if (!truncatedTail_) return;
    auto& active = segments_.back();
    const auto size = wal_segment_layout::HEADER_SIZE
        + (logicalEnd_ - active.header.startLsn);
    if (::ftruncate(activeDescriptor_, static_cast<off_t>(size)) != 0) {
        throwIo("Could not truncate active WAL segment tail");
    }
    syncFile(activeDescriptor_, &stats_);
    active.physicalBytes = size;
    active.logicalEndLsn = logicalEnd_;
    truncatedTail_ = false;
}

WalScanResult SegmentedWalStorage::scanImpl(Lsn startLsn) const {
    if (segments_.empty()) throw std::logic_error("Segmented WAL has no segments");
    const auto oldest = segments_.front().header.startLsn;
    if (startLsn < oldest || startLsn > logicalEnd_) {
        throw WalError(WalErrorKind::InvalidArgument,
                       "Required logical WAL scan start is not retained");
    }
    WalScanResult result;
    result.startOffset = startLsn;
    result.validBytes = logicalEnd_;
    result.fileBytes = logicalEnd_;
    bool boundary = startLsn == logicalEnd_;
    for (std::size_t index = 0; index < segments_.size(); ++index) {
        const auto scanned = scanOneSegment(segments_[index], index + 1 == segments_.size());
        if (scanned.truncatedTail) {
            result.truncatedTail = true;
            result.validBytes = scanned.validEnd;
            result.fileBytes = scanned.physicalLogicalEnd;
        }
        for (const auto& record : scanned.records) {
            if (record.lsn == startLsn) boundary = true;
            if (record.lsn >= startLsn) result.records.push_back(record);
        }
    }
    if (!boundary) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "Logical WAL scan start is not a record boundary");
    }
    return result;
}

WalScanResult SegmentedWalStorage::scan() const {
    return scanImpl(segments_.front().header.startLsn);
}

WalScanResult SegmentedWalStorage::scanFrom(Lsn startLsn) const {
    return scanImpl(startLsn);
}

LogRecord SegmentedWalStorage::readRecordAt(Lsn lsn) const {
    for (const auto& segment : segments_) {
        if (lsn < segment.header.startLsn || lsn >= segment.logicalEndLsn) continue;
        const auto descriptor = ::open(segment.path.c_str(), O_RDONLY);
        if (descriptor < 0) throwIo("Could not open WAL segment for record read");
        const Descriptor owned(descriptor);
        const auto local = wal_segment_layout::HEADER_SIZE + (lsn - segment.header.startLsn);
        const auto size = descriptorSize(descriptor);
        if (local > size || size - local < wal_record_layout::HEADER_SIZE) break;
        std::array<std::byte, wal_record_layout::HEADER_SIZE> header{};
        readExact(descriptor, local, header);
        const auto total = byte_codec::readUint32(header, wal_record_layout::TOTAL_LENGTH_OFFSET);
        if (total > size - local || total > wal_record_layout::MAX_RECORD_SIZE) break;
        std::vector<std::byte> encoded(total);
        readExact(descriptor, local, encoded);
        return decodeWalRecord(encoded, lsn);
    }
    throw WalError(WalErrorKind::CorruptRecord, "Referenced logical WAL record is not retained");
}

bool SegmentedWalStorage::containsLsn(Lsn lsn) const noexcept {
    if (!isValidLsn(lsn)) return false;
    try {
        static_cast<void>(readRecordAt(lsn));
        return true;
    } catch (...) {
        return false;
    }
}

std::uint64_t SegmentedWalStorage::physicalBytes() const {
    std::uint64_t total = wal_manifest_layout::HEADER_SIZE;
    for (const auto& segment : segments_) total += segment.physicalBytes;
    return total;
}

SegmentedWalStats SegmentedWalStorage::stats() const noexcept {
    auto result = stats_;
    result.retainedSegments = segments_.size();
    result.activeSegmentId = segments_.empty()
        ? INVALID_WAL_SEGMENT_ID : segments_.back().header.segmentId;
    result.oldestRetainedLsn = segments_.empty()
        ? INVALID_LSN : segments_.front().header.startLsn;
    result.logicalWalEnd = logicalEnd_;
    result.physicalWalBytes = physicalBytes();
    return result;
}

std::uint64_t SegmentedWalStorage::reclaimBefore(
    Lsn floorLsn, std::size_t extraSegments) {
    if (segments_.size() <= 1 || floorLsn < segments_.front().header.startLsn
        || floorLsn > logicalEnd_) return 0;
    std::size_t floorIndex = 0;
    while (floorIndex + 1 < segments_.size()
           && segments_[floorIndex + 1].header.startLsn <= floorLsn) {
        ++floorIndex;
    }
    const auto removable = floorIndex > extraSegments ? floorIndex - extraSegments : 0;
    if (removable == 0) return 0;

    // Publish the new recovery floor first. A crash before unlink merely leaves
    // harmless extra historical files; it can never make required files vanish.
    manifest_.firstRetainedSegmentId = segments_[removable].header.segmentId;
    publishManifest();
    recoveryFailPoint("wal_reclaim_after_manifest");

    std::uint64_t reclaimed = 0;
    for (std::size_t index = 0; index < removable; ++index) {
        reclaimed += segments_[index].physicalBytes;
        if (::unlink(segments_[index].path.c_str()) != 0) {
            throwIo("Could not delete obsolete WAL segment");
        }
        ++stats_.segmentsDeleted;
        recoveryFailPoint("wal_reclaim_after_unlink");
    }
    syncDirectory(directory_);
    ++stats_.segmentDirectorySyncs;
    stats_.walBytesReclaimed += reclaimed;
    segments_.erase(segments_.begin(), segments_.begin() + static_cast<std::ptrdiff_t>(removable));
    return reclaimed;
}

void migrateLegacyWalToSegments(
    const std::string& legacyWalPath,
    const std::string& segmentedDirectory,
    std::uint32_t payloadCapacity,
    Lsn migrationBaseLsn) {
    if (std::filesystem::exists(segmentedDirectory)) return;
    const auto scan = scanWalFile(legacyWalPath);
    if (scan.truncatedTail) {
        throw WalError(WalErrorKind::TruncatedTail,
                       "Cannot migrate a legacy WAL with an incomplete tail");
    }
    const auto temporary = segmentedDirectory + ".tmp";
    auto first = scan.records.begin();
    if (migrationBaseLsn != INVALID_LSN) {
        first = std::lower_bound(
            scan.records.begin(), scan.records.end(), migrationBaseLsn,
            [](const LogRecord& record, Lsn lsn) { return record.lsn < lsn; });
        if (first == scan.records.end() || first->lsn != migrationBaseLsn) {
            throw WalError(WalErrorKind::InvalidArgument,
                           "Legacy migration base is not a WAL record boundary");
        }
    }
    const auto migrationBase = first == scan.records.end()
        ? wal_file_layout::HEADER_SIZE : first->lsn;
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    recoveryFailPoint("wal_migration_after_temp_cleanup");
    {
        SegmentedWalStorage storage(temporary, payloadCapacity, true, migrationBase);
        recoveryFailPoint("wal_migration_after_temp_create");
        for (auto record = first; record != scan.records.end(); ++record) {
            storage.appendRecord(record->lsn, encodeWalRecord(*record, record->lsn));
            if (record == first) recoveryFailPoint("wal_migration_after_first_record");
        }
        storage.flush();
        recoveryFailPoint("wal_migration_after_final_record");
    }
    syncDirectory(temporary);
    recoveryFailPoint("wal_migration_after_temp_sync");
    if (::rename(temporary.c_str(), segmentedDirectory.c_str()) != 0) {
        throwIo("Could not publish segmented WAL directory");
    }
    recoveryFailPoint("wal_migration_after_rename");
    auto parent = std::filesystem::path(segmentedDirectory).parent_path();
    if (parent.empty()) parent = ".";
    syncDirectory(parent.string());
    recoveryFailPoint("wal_migration_after_parent_sync");
}

} // namespace minidb
