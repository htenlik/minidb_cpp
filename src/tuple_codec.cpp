#include "minidb/tuple_codec.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/slotted_page.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace minidb {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

std::size_t nullBitmapSize(std::size_t columnCount) noexcept {
    return (columnCount + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
}

void setNull(TupleBytes& bytes, std::size_t bitmapOffset, std::size_t columnIndex) {
    const auto byteOffset = bitmapOffset + (columnIndex / BITS_PER_BYTE);
    const auto bit = static_cast<std::uint8_t>(1U << (columnIndex % BITS_PER_BYTE));
    bytes[byteOffset] = static_cast<std::byte>(
        std::to_integer<std::uint8_t>(bytes[byteOffset]) | bit);
}

bool isNull(std::span<const std::byte> bytes, std::size_t bitmapOffset, std::size_t index) {
    const auto value = std::to_integer<std::uint8_t>(
        bytes[bitmapOffset + (index / BITS_PER_BYTE)]);
    return (value & static_cast<std::uint8_t>(1U << (index % BITS_PER_BYTE))) != 0;
}

void appendString(TupleBytes& bytes, const std::string& value) {
    byte_codec::appendUint32(bytes, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value) {
        bytes.push_back(static_cast<std::byte>(byte));
    }
}

} // namespace

TupleBytes TupleCodec::encode(const Schema& schema, const RowValues& values) {
    schema.validateValues(values);
    using namespace tuple_encoding_layout;
    const auto bitmapSize = nullBitmapSize(schema.columnCount());
    TupleBytes bytes(HEADER_SIZE + bitmapSize, std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(bytes, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    byte_codec::writeUint16(
        bytes, COLUMN_COUNT_OFFSET, static_cast<std::uint16_t>(schema.columnCount()));
    byte_codec::writeUint16(
        bytes, NULL_BITMAP_SIZE_OFFSET, static_cast<std::uint16_t>(bitmapSize));

    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& value = values[index];
        if (std::holds_alternative<std::monostate>(value)) {
            setNull(bytes, HEADER_SIZE, index);
            continue;
        }
        switch (schema.column(index).type) {
        case DataType::UINT32:
            byte_codec::appendUint32(bytes, std::get<std::uint32_t>(value));
            break;
        case DataType::INT64:
            byte_codec::appendUint64(
                bytes, std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value)));
            break;
        case DataType::BOOLEAN:
            bytes.push_back(std::get<bool>(value) ? std::byte{1} : std::byte{0});
            break;
        case DataType::VARCHAR:
            appendString(bytes, std::get<std::string>(value));
            break;
        }
        if (bytes.size() > slotted_page_layout::MAX_TUPLE_SIZE) {
            throw std::invalid_argument("Encoded logical tuple exceeds TupleStore inline capacity.");
        }
    }
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Encoded logical tuple exceeds its format size limit.");
    }
    byte_codec::writeUint32(bytes, ENCODED_SIZE_OFFSET, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

RowValues TupleCodec::decode(const Schema& schema, std::span<const std::byte> bytes) {
    using namespace tuple_encoding_layout;
    if (bytes.size() < HEADER_SIZE || bytes.size() > slotted_page_layout::MAX_TUPLE_SIZE
        || !std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid logical tuple magic or size.");
    }
    if (byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION) {
        throw std::runtime_error("Unsupported logical tuple encoding version.");
    }
    const auto bitmapSize = byte_codec::readUint16(bytes, NULL_BITMAP_SIZE_OFFSET);
    if (byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint16(bytes, COLUMN_COUNT_OFFSET) != schema.columnCount()
        || bitmapSize != nullBitmapSize(schema.columnCount())
        || byte_codec::readUint32(bytes, ENCODED_SIZE_OFFSET) != bytes.size()) {
        throw std::runtime_error("Logical tuple header does not match its schema or byte size.");
    }
    byte_codec::requireRange(bytes, HEADER_SIZE, bitmapSize);
    const auto unusedBits = bitmapSize * BITS_PER_BYTE - schema.columnCount();
    if (unusedBits != 0) {
        const auto last = std::to_integer<std::uint8_t>(bytes[HEADER_SIZE + bitmapSize - 1]);
        const auto validBits = static_cast<std::uint8_t>(BITS_PER_BYTE - unusedBits);
        const auto invalidMask = static_cast<std::uint8_t>(0xFFU << validBits);
        if ((last & invalidMask) != 0) {
            throw std::runtime_error("Logical tuple null bitmap has nonzero unused bits.");
        }
    }

    RowValues values;
    values.reserve(schema.columnCount());
    std::size_t cursor = HEADER_SIZE + bitmapSize;
    for (std::size_t index = 0; index < schema.columnCount(); ++index) {
        const auto& column = schema.column(index);
        if (isNull(bytes, HEADER_SIZE, index)) {
            if (!column.nullable) {
                throw std::runtime_error("Encoded tuple contains NULL in a non-nullable column.");
            }
            values.emplace_back(std::monostate{});
            continue;
        }
        switch (column.type) {
        case DataType::UINT32:
            values.emplace_back(byte_codec::readUint32(bytes, cursor));
            cursor += 4;
            break;
        case DataType::INT64:
            values.emplace_back(std::bit_cast<std::int64_t>(byte_codec::readUint64(bytes, cursor)));
            cursor += 8;
            break;
        case DataType::BOOLEAN: {
            byte_codec::requireRange(bytes, cursor, 1);
            const auto encoded = std::to_integer<std::uint8_t>(bytes[cursor++]);
            if (encoded > 1) {
                throw std::runtime_error("Encoded BOOLEAN is neither zero nor one.");
            }
            values.emplace_back(encoded == 1);
            break;
        }
        case DataType::VARCHAR: {
            const auto length = byte_codec::readUint32(bytes, cursor);
            cursor += 4;
            if (length > column.varcharMaxBytes) {
                throw std::runtime_error("Encoded VARCHAR exceeds its schema byte limit.");
            }
            byte_codec::requireRange(bytes, cursor, length);
            std::string value(length, '\0');
            for (std::size_t byte = 0; byte < length; ++byte) {
                value[byte] = static_cast<char>(
                    std::to_integer<unsigned char>(bytes[cursor + byte]));
            }
            cursor += length;
            values.emplace_back(std::move(value));
            break;
        }
        }
    }
    if (cursor != bytes.size()) {
        throw std::runtime_error("Logical tuple contains trailing unexplained bytes.");
    }
    try {
        schema.validateValues(values);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(std::string("Decoded tuple is invalid: ") + error.what());
    }
    return values;
}

} // namespace minidb
