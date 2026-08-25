#pragma once

#include "minidb/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace minidb {

namespace checkpoint_begin_log_layout {
inline constexpr std::size_t CHECKPOINT_ID_OFFSET = 0;
inline constexpr std::size_t PREVIOUS_END_LSN_OFFSET = 8;
inline constexpr std::size_t WAL_START_OFFSET = 16;
inline constexpr std::size_t RESERVED_OFFSET = 24;
inline constexpr std::size_t PAYLOAD_SIZE = 32;
}

namespace checkpoint_end_log_layout {
inline constexpr std::size_t CHECKPOINT_ID_OFFSET = 0;
inline constexpr std::size_t BEGIN_LSN_OFFSET = 8;
inline constexpr std::size_t DATABASE_PAGE_COUNT_OFFSET = 16;
inline constexpr std::size_t NEXT_TRANSACTION_ID_OFFSET = 24;
inline constexpr std::size_t RECOVERY_START_OFFSET = 32;
inline constexpr std::size_t RESERVED_OFFSET = 40;
inline constexpr std::size_t PAYLOAD_SIZE = 48;
}

struct CheckpointBeginLogPayload {
    CheckpointId checkpointId = INVALID_CHECKPOINT_ID;
    Lsn previousCheckpointEndLsn = INVALID_LSN;
    WalOffset walStartOffset = INVALID_WAL_OFFSET;
    bool operator==(const CheckpointBeginLogPayload&) const = default;
};

struct CheckpointEndLogPayload {
    CheckpointId checkpointId = INVALID_CHECKPOINT_ID;
    Lsn checkpointBeginLsn = INVALID_LSN;
    std::uint64_t databasePageCount = 0;
    TransactionId nextTransactionId = INVALID_TRANSACTION_ID;
    WalOffset recoveryStartOffset = INVALID_WAL_OFFSET;
    bool operator==(const CheckpointEndLogPayload&) const = default;
};

[[nodiscard]] std::vector<std::byte> encodeCheckpointBeginLogPayload(
    const CheckpointBeginLogPayload& payload);
[[nodiscard]] CheckpointBeginLogPayload decodeCheckpointBeginLogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodeCheckpointEndLogPayload(
    const CheckpointEndLogPayload& payload);
[[nodiscard]] CheckpointEndLogPayload decodeCheckpointEndLogPayload(
    std::span<const std::byte> bytes);
void validateCheckpointRecord(const LogRecord& record);

} // namespace minidb
