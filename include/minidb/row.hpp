#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace minidb {

struct Row {
    std::uint32_t id{};
    std::string username;
    std::string email;

    bool operator==(const Row&) const = default;
};

namespace row_layout {

inline constexpr std::size_t ID_SIZE = sizeof(std::uint32_t);
inline constexpr std::size_t USERNAME_LENGTH_SIZE = sizeof(std::uint8_t);
inline constexpr std::size_t USERNAME_MAX_SIZE = 32;
inline constexpr std::size_t EMAIL_LENGTH_SIZE = sizeof(std::uint16_t);
inline constexpr std::size_t EMAIL_MAX_SIZE = 255;

inline constexpr std::size_t ID_OFFSET = 0;
inline constexpr std::size_t USERNAME_LENGTH_OFFSET = ID_OFFSET + ID_SIZE;
inline constexpr std::size_t USERNAME_OFFSET = USERNAME_LENGTH_OFFSET + USERNAME_LENGTH_SIZE;
inline constexpr std::size_t EMAIL_LENGTH_OFFSET = USERNAME_OFFSET + USERNAME_MAX_SIZE;
inline constexpr std::size_t EMAIL_OFFSET = EMAIL_LENGTH_OFFSET + EMAIL_LENGTH_SIZE;
inline constexpr std::size_t SERIALIZED_SIZE = EMAIL_OFFSET + EMAIL_MAX_SIZE;

} // namespace row_layout

using SerializedRow = std::array<std::byte, row_layout::SERIALIZED_SIZE>;

[[nodiscard]] SerializedRow serializeRow(const Row& row);
[[nodiscard]] Row deserializeRow(std::span<const std::byte> bytes);

} // namespace minidb
