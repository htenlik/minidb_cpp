#pragma once

#include "minidb/database_format.hpp"
#include "minidb/wal_types.hpp"

#include <cstdint>
#include <string_view>

namespace minidb {

enum class CheckpointMode : std::uint8_t {
    Sharp,
    Fuzzy,
};

[[nodiscard]] constexpr std::string_view checkpointModeName(CheckpointMode mode) noexcept {
    switch (mode) {
    case CheckpointMode::Sharp: return "sharp";
    case CheckpointMode::Fuzzy: return "fuzzy";
    }
    return "unknown";
}

struct DirtyPageEntry {
    PageId pageId = INVALID_PAGE_ID;
    Lsn recLsn = INVALID_LSN;
    Lsn pageLsn = INVALID_LSN; // volatile diagnostic only; not encoded in the DPT entry
    bool operator==(const DirtyPageEntry&) const = default;
};

enum class CheckpointTransactionStatus : std::uint16_t {
    Active = 1,
};

struct CheckpointTransactionEntry {
    TransactionId transactionId = INVALID_TRANSACTION_ID;
    CheckpointTransactionStatus status = CheckpointTransactionStatus::Active;
    Lsn beginLsn = INVALID_LSN;
    Lsn lastLsn = INVALID_LSN;
    std::uint64_t startPageCount = 0;
    bool operator==(const CheckpointTransactionEntry&) const = default;
};

} // namespace minidb
