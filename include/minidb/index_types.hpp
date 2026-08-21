#pragma once

#include "minidb/record_id.hpp"

#include <cstdint>

namespace minidb {

using IndexKey = std::uint32_t;

struct IndexEntry {
    IndexKey key{};
    RecordId recordId{};

    bool operator==(const IndexEntry&) const = default;
};

} // namespace minidb
