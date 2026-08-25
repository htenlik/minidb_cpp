#include "minidb/wal.hpp"

#include "minidb/byte_codec.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace minidb {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~FileDescriptor() { if (descriptor_ >= 0) ::close(descriptor_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

[[noreturn]] void throwIo(const std::string& operation) {
    throw WalError(
        WalErrorKind::Io,
        operation + ": " + std::strerror(errno));
}

std::uint64_t fileSize(int descriptor) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) throwIo("Could not inspect WAL file");
    if (status.st_size < 0) {
        throw WalError(WalErrorKind::Io, "WAL file has a negative size");
    }
    return static_cast<std::uint64_t>(status.st_size);
}

void readExact(
    int descriptor,
    std::uint64_t offset,
    std::span<std::byte> output) {
    std::size_t completed = 0;
    while (completed < output.size()) {
        const auto result = ::pread(
            descriptor,
            output.data() + completed,
            output.size() - completed,
            static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) continue;
            throwIo("Could not read WAL file");
        }
        if (result == 0) {
            throw WalError(WalErrorKind::Io, "Unexpected end of WAL file");
        }
        completed += static_cast<std::size_t>(result);
    }
}

std::uint32_t recordChecksum(std::span<const std::byte> bytes) noexcept {
    constexpr std::uint32_t POLYNOMIAL = 0x82F63B78U;
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = index >= wal_record_layout::CHECKSUM_OFFSET
                && index < wal_record_layout::CHECKSUM_OFFSET + 4
            ? std::uint8_t{0}
            : std::to_integer<std::uint8_t>(bytes[index]);
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (POLYNOMIAL & mask);
        }
    }
    return ~crc;
}

void validateRecordPrefix(std::span<const std::byte> header, Lsn physicalLsn) {
    using namespace wal_record_layout;
    if (header.size() < HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord, "WAL record header is truncated");
    }
    if (!std::equal(MAGIC.begin(), MAGIC.end(), header.begin() + MAGIC_OFFSET)) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid WAL record magic");
    }
    if (byte_codec::readUint16(header, VERSION_OFFSET) != CURRENT_VERSION) {
        throw WalError(WalErrorKind::CorruptRecord, "Unsupported WAL record version");
    }
    const auto type = static_cast<LogRecordType>(
        byte_codec::readUint16(header, TYPE_OFFSET));
    if (!isValidLogRecordType(type)) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid WAL record type ID");
    }
    const auto totalLength = byte_codec::readUint32(header, TOTAL_LENGTH_OFFSET);
    const auto payloadLength = byte_codec::readUint32(header, PAYLOAD_LENGTH_OFFSET);
    if (totalLength < HEADER_SIZE || totalLength > MAX_RECORD_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid WAL record total length");
    }
    if (payloadLength > MAX_PAYLOAD_SIZE
        || static_cast<std::size_t>(payloadLength) != totalLength - HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord, "WAL payload/total length mismatch");
    }
    if (byte_codec::readUint64(header, LSN_OFFSET) != physicalLsn
        || physicalLsn < wal_file_layout::HEADER_SIZE || physicalLsn == INVALID_LSN) {
        throw WalError(WalErrorKind::CorruptRecord, "WAL record LSN disagrees with its offset");
    }
    if (byte_codec::readUint32(header, FLAGS_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptRecord, "WAL record reserved flags are nonzero");
    }
    const auto previous = byte_codec::readUint64(header, PREVIOUS_LSN_OFFSET);
    if (isValidLsn(previous) && previous >= physicalLsn) {
        throw WalError(WalErrorKind::CorruptRecord, "WAL prevLSN is not earlier than its record");
    }
}

WalScanResult scanDescriptor(int descriptor, WalOffset startOffset) {
    WalScanResult result;
    result.fileBytes = fileSize(descriptor);
    if (result.fileBytes < wal_file_layout::HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL file header is truncated");
    }
    std::array<std::byte, wal_file_layout::HEADER_SIZE> fileHeader{};
    readExact(descriptor, 0, fileHeader);
    validateWalFileHeader(fileHeader);

    if (startOffset < wal_file_layout::HEADER_SIZE || startOffset > result.fileBytes) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL scan offset is outside the file");
    }
    result.startOffset = startOffset;
    std::uint64_t offset = startOffset;
    while (offset < result.fileBytes) {
        const auto remaining = result.fileBytes - offset;
        if (remaining < wal_record_layout::HEADER_SIZE) {
            result.truncatedTail = true;
            break;
        }
        std::array<std::byte, wal_record_layout::HEADER_SIZE> recordHeader{};
        readExact(descriptor, offset, recordHeader);
        validateRecordPrefix(recordHeader, offset);
        const auto totalLength = byte_codec::readUint32(
            recordHeader, wal_record_layout::TOTAL_LENGTH_OFFSET);
        if (totalLength > remaining) {
            result.truncatedTail = true;
            break;
        }
        std::vector<std::byte> encoded(totalLength);
        std::copy(recordHeader.begin(), recordHeader.end(), encoded.begin());
        if (totalLength > recordHeader.size()) {
            readExact(
                descriptor,
                offset + recordHeader.size(),
                std::span<std::byte>(encoded).subspan(recordHeader.size()));
        }
        result.records.push_back(decodeWalRecord(encoded, offset));
        offset += totalLength;
    }
    result.validBytes = offset;
    return result;
}

} // namespace

std::string walPathForDatabase(std::string_view databasePath) {
    return std::string(databasePath) + ".wal";
}

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    constexpr std::uint32_t POLYNOMIAL = 0x82F63B78U;
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (POLYNOMIAL & mask);
        }
    }
    return ~crc;
}

std::array<std::byte, wal_file_layout::HEADER_SIZE> encodeWalFileHeader() {
    std::array<std::byte, wal_file_layout::HEADER_SIZE> header{};
    std::copy(
        wal_file_layout::MAGIC.begin(),
        wal_file_layout::MAGIC.end(),
        header.begin() + wal_file_layout::MAGIC_OFFSET);
    byte_codec::writeUint32(
        header, wal_file_layout::VERSION_OFFSET, wal_file_layout::CURRENT_VERSION);
    byte_codec::writeUint32(
        header,
        wal_file_layout::HEADER_SIZE_OFFSET,
        static_cast<std::uint32_t>(wal_file_layout::HEADER_SIZE));
    byte_codec::writeUint32(
        header,
        wal_file_layout::PAGE_SIZE_OFFSET,
        static_cast<std::uint32_t>(database_format::PAGE_SIZE));
    return header;
}

void validateWalFileHeader(std::span<const std::byte> bytes) {
    using namespace wal_file_layout;
    if (bytes.size() != HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL file header has the wrong size");
    }
    if (!std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET)) {
        throw WalError(WalErrorKind::CorruptHeader, "Invalid WAL file magic");
    }
    if (byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION) {
        throw WalError(WalErrorKind::CorruptHeader, "Unsupported WAL file version");
    }
    if (byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptHeader, "Invalid WAL file header size");
    }
    if (byte_codec::readUint32(bytes, PAGE_SIZE_OFFSET) != database_format::PAGE_SIZE) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL page size does not match MiniDB++");
    }
    if (byte_codec::readUint32(bytes, FLAGS_OFFSET) != 0
        || !std::all_of(
            bytes.begin() + RESERVED_OFFSET,
            bytes.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw WalError(WalErrorKind::CorruptHeader, "WAL header reserved bytes are nonzero");
    }
}

std::vector<std::byte> encodeWalRecord(const LogRecord& record, Lsn lsn) {
    using namespace wal_record_layout;
    if (!isValidLogRecordType(record.type)) {
        throw WalError(WalErrorKind::InvalidArgument, "Cannot encode invalid WAL record type");
    }
    if (!isValidLsn(lsn) || lsn < wal_file_layout::HEADER_SIZE) {
        throw WalError(WalErrorKind::InvalidArgument, "Cannot encode invalid WAL record LSN");
    }
    if (record.payload.size() > MAX_PAYLOAD_SIZE) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL record exceeds maximum size");
    }
    if (isValidLsn(record.prevLsn) && record.prevLsn >= lsn) {
        throw WalError(WalErrorKind::InvalidArgument, "WAL prevLSN must precede its record");
    }
    const auto totalLength = HEADER_SIZE + record.payload.size();
    std::vector<std::byte> encoded(totalLength);
    std::copy(MAGIC.begin(), MAGIC.end(), encoded.begin() + MAGIC_OFFSET);
    byte_codec::writeUint16(encoded, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint16(
        encoded, TYPE_OFFSET, static_cast<std::uint16_t>(record.type));
    byte_codec::writeUint32(
        encoded, TOTAL_LENGTH_OFFSET, static_cast<std::uint32_t>(totalLength));
    byte_codec::writeUint32(
        encoded,
        PAYLOAD_LENGTH_OFFSET,
        static_cast<std::uint32_t>(record.payload.size()));
    byte_codec::writeUint64(encoded, LSN_OFFSET, lsn);
    byte_codec::writeUint64(encoded, TRANSACTION_ID_OFFSET, record.transactionId);
    byte_codec::writeUint64(encoded, PREVIOUS_LSN_OFFSET, record.prevLsn);
    std::copy(record.payload.begin(), record.payload.end(), encoded.begin() + HEADER_SIZE);
    byte_codec::writeUint32(encoded, CHECKSUM_OFFSET, recordChecksum(encoded));
    return encoded;
}

LogRecord decodeWalRecord(std::span<const std::byte> bytes, Lsn physicalLsn) {
    validateRecordPrefix(bytes, physicalLsn);
    const auto totalLength = byte_codec::readUint32(
        bytes, wal_record_layout::TOTAL_LENGTH_OFFSET);
    if (bytes.size() != totalLength) {
        throw WalError(WalErrorKind::CorruptRecord, "Encoded WAL record length is inconsistent");
    }
    const auto expected = byte_codec::readUint32(bytes, wal_record_layout::CHECKSUM_OFFSET);
    if (recordChecksum(bytes) != expected) {
        throw WalError(WalErrorKind::CorruptRecord, "WAL record checksum mismatch");
    }
    LogRecord record;
    record.type = static_cast<LogRecordType>(
        byte_codec::readUint16(bytes, wal_record_layout::TYPE_OFFSET));
    record.transactionId = byte_codec::readUint64(
        bytes, wal_record_layout::TRANSACTION_ID_OFFSET);
    record.prevLsn = byte_codec::readUint64(
        bytes, wal_record_layout::PREVIOUS_LSN_OFFSET);
    record.lsn = physicalLsn;
    record.payload.assign(
        bytes.begin() + wal_record_layout::HEADER_SIZE,
        bytes.end());
    return record;
}

WalScanResult scanWalFile(const std::string& path) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throwIo("Could not open WAL file for scanning");
    const FileDescriptor owned(descriptor);
    return scanDescriptor(owned.get(), wal_file_layout::HEADER_SIZE);
}

WalScanResult scanWalFileFrom(const std::string& path, WalOffset startOffset) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throwIo("Could not open WAL file for scanning");
    const FileDescriptor owned(descriptor);
    return scanDescriptor(owned.get(), startOffset);
}

LogRecord readWalRecordAt(const std::string& path, Lsn lsn) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throwIo("Could not open WAL file for record read");
    const FileDescriptor owned(descriptor);
    const auto size = fileSize(owned.get());
    if (lsn < wal_file_layout::HEADER_SIZE
        || lsn > size
        || size - lsn < wal_record_layout::HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "Referenced WAL record header is outside the file");
    }
    std::array<std::byte, wal_file_layout::HEADER_SIZE> fileHeader{};
    readExact(owned.get(), 0, fileHeader);
    validateWalFileHeader(fileHeader);
    std::array<std::byte, wal_record_layout::HEADER_SIZE> recordHeader{};
    readExact(owned.get(), lsn, recordHeader);
    validateRecordPrefix(recordHeader, lsn);
    const auto totalLength = byte_codec::readUint32(
        recordHeader, wal_record_layout::TOTAL_LENGTH_OFFSET);
    if (totalLength > size - lsn) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "Referenced WAL record is truncated");
    }
    std::vector<std::byte> encoded(totalLength);
    std::copy(recordHeader.begin(), recordHeader.end(), encoded.begin());
    if (encoded.size() > recordHeader.size()) {
        readExact(owned.get(), lsn + recordHeader.size(),
                  std::span<std::byte>(encoded).subspan(recordHeader.size()));
    }
    return decodeWalRecord(encoded, lsn);
}

} // namespace minidb
