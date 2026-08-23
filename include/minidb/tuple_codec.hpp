#pragma once

#include "minidb/schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace minidb {

namespace tuple_encoding_layout {

using EncodingVersion = std::uint32_t;
inline constexpr EncodingVersion CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'T'},
    std::byte{'U'}, std::byte{'P'}, std::byte{'L'}, std::byte{'E'},
};
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t COLUMN_COUNT_OFFSET = 16;
inline constexpr std::size_t NULL_BITMAP_SIZE_OFFSET = 18;
inline constexpr std::size_t ENCODED_SIZE_OFFSET = 20;
inline constexpr std::size_t HEADER_SIZE = 24;

} // namespace tuple_encoding_layout

class TupleCodec {
public:
    [[nodiscard]] static TupleBytes encode(const Schema& schema, const RowValues& values);
    [[nodiscard]] static RowValues decode(
        const Schema& schema,
        std::span<const std::byte> bytes);
};

} // namespace minidb
