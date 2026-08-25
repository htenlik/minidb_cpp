#pragma once

#include "minidb/checkpoint_log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {

namespace checkpoint_control_layout {
inline constexpr std::array<std::byte, 8> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'C'},
    std::byte{'K'}, std::byte{'P'}, std::byte{'T'}, std::byte{'1'},
};
inline constexpr std::uint32_t CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t SLOT_SIZE_OFFSET = 16;
inline constexpr std::size_t SLOT_COUNT_OFFSET = 20;
inline constexpr std::size_t RESERVED_OFFSET = 24;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t SLOT_SIZE = 64;
inline constexpr std::size_t SLOT_COUNT = 2;
inline constexpr std::size_t FILE_SIZE = HEADER_SIZE + SLOT_SIZE * SLOT_COUNT;
}

namespace checkpoint_slot_layout {
inline constexpr std::size_t GENERATION_OFFSET = 0;
inline constexpr std::size_t CHECKPOINT_ID_OFFSET = 8;
inline constexpr std::size_t CHECKPOINT_END_LSN_OFFSET = 16;
inline constexpr std::size_t RECOVERY_START_OFFSET = 24;
inline constexpr std::size_t DATABASE_PAGE_COUNT_OFFSET = 32;
inline constexpr std::size_t NEXT_TRANSACTION_ID_OFFSET = 40;
inline constexpr std::size_t WAL_FILE_SIZE_OFFSET = 48;
inline constexpr std::size_t CHECKSUM_OFFSET = 56;
inline constexpr std::size_t FLAGS_OFFSET = 60;
inline constexpr std::size_t SIZE = 64;
}

struct CheckpointSlot {
    std::uint64_t generation = 0;
    CheckpointId checkpointId = INVALID_CHECKPOINT_ID;
    Lsn checkpointEndLsn = INVALID_LSN;
    WalOffset recoveryStartOffset = INVALID_WAL_OFFSET;
    std::uint64_t databasePageCount = 0;
    TransactionId nextTransactionId = INVALID_TRANSACTION_ID;
    std::uint64_t walFileSizeAtCheckpoint = 0;
    bool operator==(const CheckpointSlot&) const = default;
};

struct CheckpointSelection {
    bool controlFilePresent = false;
    std::uint64_t validationFailures = 0;
    std::optional<CheckpointSlot> slot;
};

struct CheckpointControlStats {
    std::uint64_t slotWrites = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t fsyncCalls = 0;
};

[[nodiscard]] std::string checkpointPathForDatabase(std::string_view databasePath);
[[nodiscard]] std::array<std::byte, checkpoint_control_layout::HEADER_SIZE>
encodeCheckpointControlHeader();
void validateCheckpointControlHeader(std::span<const std::byte> bytes);
[[nodiscard]] std::array<std::byte, checkpoint_slot_layout::SIZE>
encodeCheckpointSlot(const CheckpointSlot& slot);
[[nodiscard]] CheckpointSlot decodeCheckpointSlot(std::span<const std::byte> bytes);

class LogManager;

class CheckpointControl {
public:
    explicit CheckpointControl(std::string path) : path_(std::move(path)) {}

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] CheckpointSelection select(const LogManager& logManager) const;
    void publish(const CheckpointSlot& slot);
    [[nodiscard]] CheckpointControlStats stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = {}; }

private:
    std::string path_;
    CheckpointControlStats stats_{};

    void initializeFileIfNeeded();
};

} // namespace minidb
