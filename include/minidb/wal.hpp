#pragma once

#include "minidb/database_format.hpp"
#include "minidb/wal_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb {

namespace wal_file_layout {

inline constexpr std::array<std::byte, 8> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'W'},
    std::byte{'A'}, std::byte{'L'}, std::byte{'0'}, std::byte{'1'},
};
inline constexpr std::uint32_t CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t PAGE_SIZE_OFFSET = 16;
inline constexpr std::size_t FLAGS_OFFSET = 20;
inline constexpr std::size_t RESERVED_OFFSET = 24;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t RESERVED_SIZE = HEADER_SIZE - RESERVED_OFFSET;

static_assert(RESERVED_SIZE == 40);

} // namespace wal_file_layout

namespace wal_record_layout {

inline constexpr std::array<std::byte, 4> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'R'},
};
inline constexpr std::uint16_t CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 4;
inline constexpr std::size_t TYPE_OFFSET = 6;
inline constexpr std::size_t TOTAL_LENGTH_OFFSET = 8;
inline constexpr std::size_t PAYLOAD_LENGTH_OFFSET = 12;
inline constexpr std::size_t LSN_OFFSET = 16;
inline constexpr std::size_t TRANSACTION_ID_OFFSET = 24;
inline constexpr std::size_t PREVIOUS_LSN_OFFSET = 32;
inline constexpr std::size_t CHECKSUM_OFFSET = 40;
inline constexpr std::size_t FLAGS_OFFSET = 44;
inline constexpr std::size_t HEADER_SIZE = 48;
inline constexpr std::size_t MAX_RECORD_SIZE = 1024 * 1024;
inline constexpr std::size_t MAX_PAYLOAD_SIZE = MAX_RECORD_SIZE - HEADER_SIZE;

static_assert(MAX_RECORD_SIZE > (2 * database_format::PAGE_SIZE));

} // namespace wal_record_layout

enum class WalErrorKind {
    InvalidArgument,
    Io,
    Durability,
    CorruptHeader,
    CorruptRecord,
    TruncatedTail,
};

class WalError : public std::runtime_error {
public:
    WalError(WalErrorKind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    [[nodiscard]] WalErrorKind kind() const noexcept { return kind_; }

private:
    WalErrorKind kind_;
};

struct LogRecord {
    LogRecordType type = LogRecordType::Begin;
    TransactionId transactionId = INVALID_TRANSACTION_ID;
    Lsn prevLsn = INVALID_LSN;
    std::vector<std::byte> payload;
    Lsn lsn = INVALID_LSN;

    bool operator==(const LogRecord&) const = default;
};

struct WalScanResult {
    std::vector<LogRecord> records;
    bool truncatedTail = false;
    std::uint64_t validBytes = wal_file_layout::HEADER_SIZE;
    std::uint64_t fileBytes = wal_file_layout::HEADER_SIZE;
};

[[nodiscard]] std::string walPathForDatabase(std::string_view databasePath);

// CRC32C (Castagnoli), reflected polynomial 0x82F63B78, init/final XOR 0xFFFFFFFF.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::array<std::byte, wal_file_layout::HEADER_SIZE> encodeWalFileHeader();
void validateWalFileHeader(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encodeWalRecord(const LogRecord& record, Lsn lsn);
[[nodiscard]] LogRecord decodeWalRecord(
    std::span<const std::byte> bytes,
    Lsn physicalLsn);

[[nodiscard]] WalScanResult scanWalFile(const std::string& path);

} // namespace minidb
