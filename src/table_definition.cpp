#include "minidb/table_definition.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/slotted_page.hpp"

#include <algorithm>
#include <stdexcept>

namespace minidb {
namespace {

void validateDefinition(const TableDefinition& definition) {
    if (definition.tableId == INVALID_TABLE_ID) {
        throw std::invalid_argument("Table definition uses the invalid table ID.");
    }
    if (normalizeIdentifier(definition.name) != definition.name) {
        throw std::invalid_argument("Table definition name is not normalized.");
    }
    if (definition.heapMetadataPageId == database_format::METADATA_PAGE_ID
        || definition.heapMetadataPageId == INVALID_PAGE_ID) {
        throw std::invalid_argument("Table definition has an invalid heap metadata PageId.");
    }
    if (definition.primaryIndexMetadataPageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Table definition has an invalid index metadata PageId.");
    }
    const bool schemaHasPrimaryKey = definition.schema.primaryKeyColumn().has_value();
    const bool definitionHasIndex = definition.primaryIndexMetadataPageId != INVALID_PAGE_ID;
    if (schemaHasPrimaryKey != definitionHasIndex) {
        throw std::invalid_argument("Table definition primary-index presence disagrees with schema.");
    }
}

} // namespace

TupleBytes encodeTableDefinition(const TableDefinition& definition) {
    validateDefinition(definition);
    using namespace table_definition_layout;
    const auto encodedSchema = encodeSchema(definition.schema);
    const auto encodedSize = HEADER_SIZE + definition.name.size() + encodedSchema.size();
    if (encodedSize > slotted_page_layout::MAX_TUPLE_SIZE) {
        throw std::invalid_argument("Table definition exceeds catalog inline tuple capacity.");
    }

    TupleBytes bytes(encodedSize, std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(bytes, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    byte_codec::writeUint32(bytes, ENCODED_SIZE_OFFSET, static_cast<std::uint32_t>(bytes.size()));
    byte_codec::writeUint64(bytes, TABLE_ID_OFFSET, definition.tableId);
    byte_codec::writeUint32(bytes, HEAP_METADATA_PAGE_ID_OFFSET, definition.heapMetadataPageId);
    byte_codec::writeUint32(
        bytes, PRIMARY_INDEX_METADATA_PAGE_ID_OFFSET,
        definition.primaryIndexMetadataPageId);
    byte_codec::writeUint16(
        bytes, TABLE_NAME_LENGTH_OFFSET, static_cast<std::uint16_t>(definition.name.size()));
    byte_codec::writeUint32(
        bytes, SCHEMA_SIZE_OFFSET, static_cast<std::uint32_t>(encodedSchema.size()));
    std::size_t cursor = HEADER_SIZE;
    for (const unsigned char value : definition.name) {
        bytes[cursor++] = static_cast<std::byte>(value);
    }
    std::copy(encodedSchema.begin(), encodedSchema.end(), bytes.begin() + cursor);
    return bytes;
}

TableDefinition decodeTableDefinition(std::span<const std::byte> bytes) {
    using namespace table_definition_layout;
    if (bytes.size() < HEADER_SIZE
        || !std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid table-definition magic or size.");
    }
    if (byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION) {
        throw std::runtime_error("Unsupported table-definition version.");
    }
    if (byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint32(bytes, ENCODED_SIZE_OFFSET) != bytes.size()
        || byte_codec::readUint32(bytes, FLAGS_OFFSET) != 0
        || byte_codec::readUint16(bytes, RESERVED_OFFSET) != 0) {
        throw std::runtime_error("Table-definition header is malformed.");
    }
    const auto nameLength = byte_codec::readUint16(bytes, TABLE_NAME_LENGTH_OFFSET);
    const auto schemaSize = byte_codec::readUint32(bytes, SCHEMA_SIZE_OFFSET);
    const auto variableSize = static_cast<std::size_t>(nameLength) + schemaSize;
    if (variableSize > bytes.size() - HEADER_SIZE
        || HEADER_SIZE + variableSize != bytes.size()) {
        throw std::runtime_error("Table-definition variable fields are malformed.");
    }
    std::string name(nameLength, '\0');
    for (std::size_t index = 0; index < nameLength; ++index) {
        name[index] = static_cast<char>(
            std::to_integer<unsigned char>(bytes[HEADER_SIZE + index]));
    }
    try {
        TableDefinition definition{
            byte_codec::readUint64(bytes, TABLE_ID_OFFSET),
            std::move(name),
            decodeSchema(bytes.subspan(HEADER_SIZE + nameLength, schemaSize)),
            byte_codec::readUint32(bytes, HEAP_METADATA_PAGE_ID_OFFSET),
            byte_codec::readUint32(bytes, PRIMARY_INDEX_METADATA_PAGE_ID_OFFSET),
        };
        validateDefinition(definition);
        if (encodeTableDefinition(definition) != TupleBytes(bytes.begin(), bytes.end())) {
            throw std::runtime_error("Table-definition encoding is not canonical.");
        }
        return definition;
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(std::string("Decoded table definition is invalid: ") + error.what());
    }
}

} // namespace minidb
