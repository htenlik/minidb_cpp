#include "minidb/schema.hpp"

#include "minidb/byte_codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace minidb {
namespace {

bool isIdentifierStart(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

bool isIdentifierContinue(char value) noexcept {
    return isIdentifierStart(value) || (value >= '0' && value <= '9');
}

void validateColumnType(const ColumnDefinition& column) {
    switch (column.type) {
    case DataType::UINT32:
    case DataType::INT64:
    case DataType::BOOLEAN:
        if (column.varcharMaxBytes != 0) {
            throw std::invalid_argument("Only VARCHAR columns may specify a maximum byte length.");
        }
        break;
    case DataType::VARCHAR:
        if (column.varcharMaxBytes == 0 || column.varcharMaxBytes > MAX_VARCHAR_BYTES) {
            throw std::invalid_argument("VARCHAR maximum byte length is outside supported limits.");
        }
        break;
    default:
        throw std::invalid_argument("Column uses an unsupported data type.");
    }
}

} // namespace

std::string normalizeIdentifier(std::string_view identifier) {
    if (identifier.empty() || identifier.size() > MAX_IDENTIFIER_BYTES) {
        throw std::invalid_argument("Identifier length is outside supported limits.");
    }
    if (!isIdentifierStart(identifier.front())) {
        throw std::invalid_argument("Identifier has an invalid first character.");
    }

    std::string normalized;
    normalized.reserve(identifier.size());
    for (const char value : identifier) {
        if (!isIdentifierContinue(value)) {
            throw std::invalid_argument("Identifier contains a non-ASCII or invalid character.");
        }
        normalized.push_back(
            (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value);
    }
    return normalized;
}

Schema Schema::create(std::vector<ColumnDefinition> columns) {
    if (columns.empty() || columns.size() > MAX_SCHEMA_COLUMNS
        || columns.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("Schema column count is outside supported limits.");
    }

    std::unordered_set<std::string> names;
    std::optional<std::size_t> primaryKey;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        auto& column = columns[index];
        column.name = normalizeIdentifier(column.name);
        if (!names.insert(column.name).second) {
            throw std::invalid_argument("Schema contains duplicate normalized column names.");
        }
        validateColumnType(column);
        if (column.primaryKey) {
            if (primaryKey.has_value()) {
                throw std::invalid_argument("Schema contains more than one primary key.");
            }
            if (column.nullable) {
                throw std::invalid_argument("Primary-key column cannot be nullable.");
            }
            if (column.type != DataType::UINT32) {
                throw std::invalid_argument("Milestone 5B primary keys must use UINT32.");
            }
            primaryKey = index;
        }
    }
    return Schema(std::move(columns));
}

const ColumnDefinition& Schema::column(std::size_t index) const {
    if (index >= columns_.size()) {
        throw std::out_of_range("Schema column index is out of range.");
    }
    return columns_[index];
}

std::optional<std::size_t> Schema::findColumn(std::string_view name) const {
    const auto normalized = normalizeIdentifier(name);
    const auto found = std::find_if(
        columns_.begin(), columns_.end(),
        [&](const ColumnDefinition& column) { return column.name == normalized; });
    if (found == columns_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(columns_.begin(), found));
}

std::optional<std::size_t> Schema::primaryKeyColumn() const noexcept {
    for (std::size_t index = 0; index < columns_.size(); ++index) {
        if (columns_[index].primaryKey) {
            return index;
        }
    }
    return std::nullopt;
}

void Schema::validateValues(const RowValues& values) const {
    if (values.size() != columns_.size()) {
        throw std::invalid_argument("Logical row value count does not match its schema.");
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& column = columns_[index];
        const auto& value = values[index];
        if (std::holds_alternative<std::monostate>(value)) {
            if (!column.nullable) {
                throw std::invalid_argument("NULL supplied for a non-nullable column.");
            }
            continue;
        }
        const bool typeMatches =
            (column.type == DataType::UINT32 && std::holds_alternative<std::uint32_t>(value))
            || (column.type == DataType::INT64 && std::holds_alternative<std::int64_t>(value))
            || (column.type == DataType::BOOLEAN && std::holds_alternative<bool>(value))
            || (column.type == DataType::VARCHAR && std::holds_alternative<std::string>(value));
        if (!typeMatches) {
            throw std::invalid_argument("Logical value type does not match its column.");
        }
        if (column.type == DataType::VARCHAR
            && std::get<std::string>(value).size() > column.varcharMaxBytes) {
            throw std::invalid_argument("VARCHAR value exceeds its declared byte limit.");
        }
    }
}

TupleBytes encodeSchema(const Schema& schema) {
    using namespace schema_encoding_layout;
    std::size_t encodedSize = HEADER_SIZE;
    for (const auto& column : schema.columns()) {
        encodedSize += COLUMN_HEADER_SIZE + column.name.size();
    }
    if (encodedSize > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Encoded schema exceeds its format size limit.");
    }

    TupleBytes bytes(encodedSize, std::byte{0});
    std::copy(std::begin(MAGIC), std::end(MAGIC), bytes.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(bytes, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    byte_codec::writeUint16(
        bytes, COLUMN_COUNT_OFFSET, static_cast<std::uint16_t>(schema.columnCount()));
    byte_codec::writeUint32(bytes, ENCODED_SIZE_OFFSET, static_cast<std::uint32_t>(bytes.size()));

    std::size_t cursor = HEADER_SIZE;
    for (const auto& column : schema.columns()) {
        byte_codec::writeUint16(
            bytes, cursor + COLUMN_NAME_LENGTH_OFFSET,
            static_cast<std::uint16_t>(column.name.size()));
        bytes[cursor + COLUMN_TYPE_OFFSET] = static_cast<std::byte>(column.type);
        std::uint8_t flags = 0;
        if (column.nullable) {
            flags |= NULLABLE_FLAG;
        }
        if (column.primaryKey) {
            flags |= PRIMARY_KEY_FLAG;
        }
        bytes[cursor + COLUMN_FLAGS_OFFSET] = static_cast<std::byte>(flags);
        byte_codec::writeUint32(bytes, cursor + COLUMN_VARCHAR_MAX_OFFSET, column.varcharMaxBytes);
        for (std::size_t index = 0; index < column.name.size(); ++index) {
            bytes[cursor + COLUMN_HEADER_SIZE + index] =
                static_cast<std::byte>(static_cast<unsigned char>(column.name[index]));
        }
        cursor += COLUMN_HEADER_SIZE + column.name.size();
    }
    return bytes;
}

Schema decodeSchema(std::span<const std::byte> bytes) {
    using namespace schema_encoding_layout;
    if (bytes.size() < HEADER_SIZE
        || !std::equal(std::begin(MAGIC), std::end(MAGIC), bytes.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid schema encoding magic.");
    }
    if (byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION) {
        throw std::runtime_error("Unsupported schema encoding version.");
    }
    if (byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint16(bytes, FLAGS_OFFSET) != 0
        || byte_codec::readUint32(bytes, ENCODED_SIZE_OFFSET) != bytes.size()) {
        throw std::runtime_error("Schema encoding header is malformed.");
    }
    const auto columnCount = byte_codec::readUint16(bytes, COLUMN_COUNT_OFFSET);
    if (columnCount == 0 || columnCount > MAX_SCHEMA_COLUMNS) {
        throw std::runtime_error("Encoded schema has an invalid column count.");
    }

    std::vector<ColumnDefinition> columns;
    columns.reserve(columnCount);
    std::size_t cursor = HEADER_SIZE;
    for (std::size_t index = 0; index < columnCount; ++index) {
        byte_codec::requireRange(bytes, cursor, COLUMN_HEADER_SIZE);
        const auto nameLength = byte_codec::readUint16(bytes, cursor + COLUMN_NAME_LENGTH_OFFSET);
        byte_codec::requireRange(bytes, cursor + COLUMN_HEADER_SIZE, nameLength);
        const auto rawType = std::to_integer<std::uint8_t>(bytes[cursor + COLUMN_TYPE_OFFSET]);
        if (rawType < static_cast<std::uint8_t>(DataType::UINT32)
            || rawType > static_cast<std::uint8_t>(DataType::VARCHAR)) {
            throw std::runtime_error("Encoded schema contains an unknown type ID.");
        }
        const auto flags = std::to_integer<std::uint8_t>(bytes[cursor + COLUMN_FLAGS_OFFSET]);
        if ((flags & static_cast<std::uint8_t>(~(NULLABLE_FLAG | PRIMARY_KEY_FLAG))) != 0) {
            throw std::runtime_error("Encoded schema contains unsupported column flags.");
        }
        std::string name(nameLength, '\0');
        for (std::size_t byte = 0; byte < nameLength; ++byte) {
            name[byte] = static_cast<char>(
                std::to_integer<unsigned char>(bytes[cursor + COLUMN_HEADER_SIZE + byte]));
        }
        columns.push_back(ColumnDefinition{
            std::move(name),
            static_cast<DataType>(rawType),
            (flags & NULLABLE_FLAG) != 0,
            (flags & PRIMARY_KEY_FLAG) != 0,
            byte_codec::readUint32(bytes, cursor + COLUMN_VARCHAR_MAX_OFFSET),
        });
        cursor += COLUMN_HEADER_SIZE + nameLength;
    }
    if (cursor != bytes.size()) {
        throw std::runtime_error("Schema encoding contains trailing bytes.");
    }
    try {
        auto schema = Schema::create(std::move(columns));
        if (encodeSchema(schema) != TupleBytes(bytes.begin(), bytes.end())) {
            throw std::runtime_error("Schema encoding is not canonical.");
        }
        return schema;
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(std::string("Encoded schema is invalid: ") + error.what());
    }
}

} // namespace minidb
