#include "minidb/row.hpp"

#include <stdexcept>

namespace minidb {
namespace {

void writeUint16LittleEndian(SerializedRow& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::byte>(value & 0xFFU);
    output[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void writeUint32LittleEndian(SerializedRow& output, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        output[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

std::uint16_t readUint16LittleEndian(std::span<const std::byte> input, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(input[offset]))
        | static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(input[offset + 1]) << 8U);
}

std::uint32_t readUint32LittleEndian(std::span<const std::byte> input, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        value |= std::to_integer<std::uint32_t>(input[offset + i]) << (i * 8U);
    }
    return value;
}

void writeString(SerializedRow& output, std::size_t offset, const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        output[offset + i] = static_cast<std::byte>(static_cast<unsigned char>(value[i]));
    }
}

std::string readString(std::span<const std::byte> input, std::size_t offset, std::size_t length) {
    std::string value(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        value[i] = static_cast<char>(std::to_integer<unsigned char>(input[offset + i]));
    }
    return value;
}

} // namespace

SerializedRow serializeRow(const Row& row) {
    if (row.username.size() > row_layout::USERNAME_MAX_SIZE) {
        throw std::invalid_argument("Username exceeds the maximum serialized size of 32 bytes.");
    }
    if (row.email.size() > row_layout::EMAIL_MAX_SIZE) {
        throw std::invalid_argument("Email exceeds the maximum serialized size of 255 bytes.");
    }

    SerializedRow output{};
    writeUint32LittleEndian(output, row_layout::ID_OFFSET, row.id);
    output[row_layout::USERNAME_LENGTH_OFFSET] = static_cast<std::byte>(row.username.size());
    writeString(output, row_layout::USERNAME_OFFSET, row.username);
    writeUint16LittleEndian(
        output,
        row_layout::EMAIL_LENGTH_OFFSET,
        static_cast<std::uint16_t>(row.email.size()));
    writeString(output, row_layout::EMAIL_OFFSET, row.email);
    return output;
}

Row deserializeRow(std::span<const std::byte> bytes) {
    if (bytes.size() != row_layout::SERIALIZED_SIZE) {
        throw std::invalid_argument("Serialized row has an invalid size.");
    }

    const auto usernameLength =
        std::to_integer<std::size_t>(bytes[row_layout::USERNAME_LENGTH_OFFSET]);
    if (usernameLength > row_layout::USERNAME_MAX_SIZE) {
        throw std::invalid_argument("Serialized row contains an invalid username length.");
    }

    const auto emailLength = readUint16LittleEndian(bytes, row_layout::EMAIL_LENGTH_OFFSET);
    if (emailLength > row_layout::EMAIL_MAX_SIZE) {
        throw std::invalid_argument("Serialized row contains an invalid email length.");
    }

    return Row{
        readUint32LittleEndian(bytes, row_layout::ID_OFFSET),
        readString(bytes, row_layout::USERNAME_OFFSET, usernameLength),
        readString(bytes, row_layout::EMAIL_OFFSET, emailLength),
    };
}

} // namespace minidb
