#include "minidb/row.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void requireInvalidArgument(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void testValidRowRoundTrip() {
    const minidb::Row original{42, "alice", "alice@example.com"};
    const auto serialized = minidb::serializeRow(original);

    static_assert(minidb::row_layout::SERIALIZED_SIZE == 294);
    require(serialized.size() == minidb::row_layout::SERIALIZED_SIZE,
            "Serialized row did not have the fixed size");
    require(minidb::deserializeRow(serialized) == original,
            "Deserialized row did not match the original");
}

void testBinaryLayoutIsDeterministic() {
    const minidb::Row row{0x12345678U, "ab", "c"};
    const auto serialized = minidb::serializeRow(row);

    require(std::to_integer<std::uint8_t>(serialized[minidb::row_layout::ID_OFFSET]) == 0x78,
            "Row ID was not encoded in little-endian order");
    require(std::to_integer<std::uint8_t>(serialized[minidb::row_layout::ID_OFFSET + 3]) == 0x12,
            "Row ID was not encoded in little-endian order");
    require(std::to_integer<std::uint8_t>(
                serialized[minidb::row_layout::USERNAME_LENGTH_OFFSET]) == 2,
            "Username length was not encoded");
    require(serialized[minidb::row_layout::USERNAME_OFFSET] == std::byte{'a'},
            "Username bytes were not stored at the documented offset");
    require(serialized[minidb::row_layout::USERNAME_OFFSET + 2] == std::byte{0},
            "Unused username capacity was not zero padded");
    require(std::to_integer<std::uint8_t>(serialized[minidb::row_layout::EMAIL_LENGTH_OFFSET]) == 1,
            "Email length was not encoded");
    require(serialized[minidb::row_layout::EMAIL_OFFSET] == std::byte{'c'},
            "Email bytes were not stored at the documented offset");
}

void testBoundaryLengths() {
    const minidb::Row boundary{
        7,
        std::string(minidb::row_layout::USERNAME_MAX_SIZE, 'u'),
        std::string(minidb::row_layout::EMAIL_MAX_SIZE, 'e'),
    };

    const auto serialized = minidb::serializeRow(boundary);
    require(minidb::deserializeRow(serialized) == boundary,
            "Maximum-length fields did not round trip");
}

void testOversizedFieldsAreRejected() {
    const minidb::Row oversizedUsername{
        1,
        std::string(minidb::row_layout::USERNAME_MAX_SIZE + 1, 'u'),
        "valid@example.com",
    };
    requireInvalidArgument(
        [&] { static_cast<void>(minidb::serializeRow(oversizedUsername)); },
        "Oversized username was accepted");

    const minidb::Row oversizedEmail{
        2,
        "valid",
        std::string(minidb::row_layout::EMAIL_MAX_SIZE + 1, 'e'),
    };
    requireInvalidArgument(
        [&] { static_cast<void>(minidb::serializeRow(oversizedEmail)); },
        "Oversized email was accepted");
}

void testRowsRemainIndependent() {
    const minidb::Row first{1, "first", "first@example.com"};
    const minidb::Row second{2, "second", "second@example.net"};
    const auto firstBytes = minidb::serializeRow(first);
    const auto secondBytes = minidb::serializeRow(second);

    require(firstBytes != secondBytes, "Different rows produced identical bytes");
    require(minidb::deserializeRow(firstBytes) == first,
            "First row changed while serializing another row");
    require(minidb::deserializeRow(secondBytes) == second,
            "Second row changed while serializing another row");
}

void testEmbeddedNullBytesRoundTrip() {
    const minidb::Row row{9, std::string{"a\0b", 3}, std::string{"x\0y", 3}};
    require(minidb::deserializeRow(minidb::serializeRow(row)) == row,
            "Explicit lengths did not preserve embedded null bytes");
}

void testMalformedInputIsRejected() {
    auto serialized = minidb::serializeRow(minidb::Row{1, "valid", "valid@example.com"});
    const auto truncated =
        std::span<const std::byte>(serialized.data(), serialized.size() - 1);
    requireInvalidArgument(
        [&] { static_cast<void>(minidb::deserializeRow(truncated)); },
        "Incorrect serialized size was accepted");

    serialized[minidb::row_layout::USERNAME_LENGTH_OFFSET] =
        static_cast<std::byte>(minidb::row_layout::USERNAME_MAX_SIZE + 1);
    requireInvalidArgument(
        [&] { static_cast<void>(minidb::deserializeRow(serialized)); },
        "Invalid serialized username length was accepted");

    serialized = minidb::serializeRow(minidb::Row{1, "valid", "valid@example.com"});
    serialized[minidb::row_layout::EMAIL_LENGTH_OFFSET] = std::byte{0};
    serialized[minidb::row_layout::EMAIL_LENGTH_OFFSET + 1] = std::byte{1};
    requireInvalidArgument(
        [&] { static_cast<void>(minidb::deserializeRow(serialized)); },
        "Invalid serialized email length was accepted");
}

} // namespace

int main() {
    try {
        testValidRowRoundTrip();
        testBinaryLayoutIsDeterministic();
        testBoundaryLengths();
        testOversizedFieldsAreRejected();
        testRowsRemainIndependent();
        testEmbeddedNullBytesRoundTrip();
        testMalformedInputIsRejected();
        std::cout << "row_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "row_test failed: " << exception.what() << '\n';
        return 1;
    }
}
