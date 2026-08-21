#pragma once

#include "minidb/database_format.hpp"

#include <cstdint>
#include <limits>

namespace minidb {

using SlotId = std::uint16_t;
inline constexpr SlotId INVALID_SLOT_ID = std::numeric_limits<SlotId>::max();

struct RecordId {
    PageId pageId = INVALID_PAGE_ID;
    SlotId slotId = INVALID_SLOT_ID;

    [[nodiscard]] bool isValid() const noexcept {
        return pageId != INVALID_PAGE_ID
            && pageId != database_format::METADATA_PAGE_ID
            && slotId != INVALID_SLOT_ID;
    }

    bool operator==(const RecordId&) const = default;
};

inline constexpr RecordId INVALID_RECORD_ID{};

} // namespace minidb
