#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace minidb::byte_codec {

inline void requireRange(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) {
    if (offset > bytes.size() || width > bytes.size() - offset) {
        throw std::runtime_error("Persistent byte sequence is truncated.");
    }
}

inline void writeUint16(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint16_t value) {
    requireRange(bytes, offset, 2);
    for (std::size_t index = 0; index < 2; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

inline void writeUint32(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint32_t value) {
    requireRange(bytes, offset, 4);
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

inline void writeUint64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) {
    requireRange(bytes, offset, 8);
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

inline std::uint16_t readUint16(std::span<const std::byte> bytes, std::size_t offset) {
    requireRange(bytes, offset, 2);
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(bytes[offset + index]) << (index * 8U));
    }
    return value;
}

inline std::uint32_t readUint32(std::span<const std::byte> bytes, std::size_t offset) {
    requireRange(bytes, offset, 4);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

inline std::uint64_t readUint64(std::span<const std::byte> bytes, std::size_t offset) {
    requireRange(bytes, offset, 8);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

inline void appendUint16(std::vector<std::byte>& bytes, std::uint16_t value) {
    const auto offset = bytes.size();
    bytes.resize(offset + 2);
    writeUint16(bytes, offset, value);
}

inline void appendUint32(std::vector<std::byte>& bytes, std::uint32_t value) {
    const auto offset = bytes.size();
    bytes.resize(offset + 4);
    writeUint32(bytes, offset, value);
}

inline void appendUint64(std::vector<std::byte>& bytes, std::uint64_t value) {
    const auto offset = bytes.size();
    bytes.resize(offset + 8);
    writeUint64(bytes, offset, value);
}

} // namespace minidb::byte_codec
