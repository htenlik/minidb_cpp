#pragma once

#include <cstdint>
#include <limits>

namespace minidb {

// FrameId names a volatile slot in buffer-pool memory. PageId names a persistent page.
using FrameId = std::uint32_t;
inline constexpr FrameId INVALID_FRAME_ID = std::numeric_limits<FrameId>::max();

} // namespace minidb
