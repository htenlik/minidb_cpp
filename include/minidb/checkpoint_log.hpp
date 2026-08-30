#pragma once

#include "minidb/checkpoint_types.hpp"
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

namespace fuzzy_checkpoint_begin_log_layout {
inline constexpr std::uint16_t CURRENT_VERSION = 1;
inline constexpr std::size_t VERSION_OFFSET = 0;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 2;
inline constexpr std::size_t FLAGS_OFFSET = 4;
inline constexpr std::size_t CHECKPOINT_ID_OFFSET = 8;
inline constexpr std::size_t PREVIOUS_END_LSN_OFFSET = 16;
inline constexpr std::size_t RESERVED_OFFSET = 24;
inline constexpr std::size_t PAYLOAD_SIZE = 32;
}

namespace fuzzy_checkpoint_end_log_layout {
inline constexpr std::uint16_t CURRENT_VERSION = 1;
inline constexpr std::size_t VERSION_OFFSET = 0;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 2;
inline constexpr std::size_t DPT_ENTRY_SIZE_OFFSET = 4;
inline constexpr std::size_t ATT_ENTRY_SIZE_OFFSET = 6;
inline constexpr std::size_t CHECKPOINT_ID_OFFSET = 8;
inline constexpr std::size_t BEGIN_LSN_OFFSET = 16;
inline constexpr std::size_t DATABASE_PAGE_COUNT_OFFSET = 24;
inline constexpr std::size_t NEXT_TRANSACTION_ID_OFFSET = 32;
inline constexpr std::size_t DPT_COUNT_OFFSET = 40;
inline constexpr std::size_t ATT_COUNT_OFFSET = 44;
inline constexpr std::size_t RESERVED_OFFSET = 48;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t DPT_ENTRY_SIZE = 16;
inline constexpr std::size_t ATT_ENTRY_SIZE = 48;
}

namespace fuzzy_checkpoint_dpt_entry_layout {
inline constexpr std::size_t PAGE_ID_OFFSET = 0;
inline constexpr std::size_t RESERVED_OFFSET = 4;
inline constexpr std::size_t REC_LSN_OFFSET = 8;
inline constexpr std::size_t SIZE = 16;
}

namespace fuzzy_checkpoint_att_entry_layout {
inline constexpr std::size_t TRANSACTION_ID_OFFSET = 0;
inline constexpr std::size_t STATUS_OFFSET = 8;
inline constexpr std::size_t RESERVED16_OFFSET = 10;
inline constexpr std::size_t RESERVED32_OFFSET = 12;
inline constexpr std::size_t BEGIN_LSN_OFFSET = 16;
inline constexpr std::size_t LAST_LSN_OFFSET = 24;
inline constexpr std::size_t START_PAGE_COUNT_OFFSET = 32;
inline constexpr std::size_t RESERVED64_OFFSET = 40;
inline constexpr std::size_t SIZE = 48;
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

struct FuzzyCheckpointBeginLogPayload {
    CheckpointId checkpointId = INVALID_CHECKPOINT_ID;
    Lsn previousCheckpointEndLsn = INVALID_LSN;
    bool operator==(const FuzzyCheckpointBeginLogPayload&) const = default;
};

struct FuzzyCheckpointEndLogPayload {
    CheckpointId checkpointId = INVALID_CHECKPOINT_ID;
    Lsn checkpointBeginLsn = INVALID_LSN;
    std::uint64_t databasePageCount = 0;
    TransactionId nextTransactionId = INVALID_TRANSACTION_ID;
    std::vector<DirtyPageEntry> dirtyPages;
    std::vector<CheckpointTransactionEntry> activeTransactions;
    bool operator==(const FuzzyCheckpointEndLogPayload&) const = default;
};

[[nodiscard]] std::vector<std::byte> encodeCheckpointBeginLogPayload(
    const CheckpointBeginLogPayload& payload);
[[nodiscard]] CheckpointBeginLogPayload decodeCheckpointBeginLogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodeCheckpointEndLogPayload(
    const CheckpointEndLogPayload& payload);
[[nodiscard]] CheckpointEndLogPayload decodeCheckpointEndLogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodeFuzzyCheckpointBeginLogPayload(
    const FuzzyCheckpointBeginLogPayload& payload);
[[nodiscard]] FuzzyCheckpointBeginLogPayload decodeFuzzyCheckpointBeginLogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodeFuzzyCheckpointEndLogPayload(
    const FuzzyCheckpointEndLogPayload& payload);
[[nodiscard]] FuzzyCheckpointEndLogPayload decodeFuzzyCheckpointEndLogPayload(
    std::span<const std::byte> bytes);
void validateCheckpointRecord(const LogRecord& record);

} // namespace minidb
