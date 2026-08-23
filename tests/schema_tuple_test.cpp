#include "minidb/byte_codec.hpp"
#include "minidb/schema.hpp"
#include "minidb/slotted_page.hpp"
#include "minidb/tuple_codec.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

minidb::Schema makeSchema() {
    return minidb::Schema::create({
        {"ID", minidb::DataType::UINT32, false, true, 0},
        {"Balance", minidb::DataType::INT64, true, false, 0},
        {"Active", minidb::DataType::BOOLEAN, false, false, 0},
        {"Display_Name", minidb::DataType::VARCHAR, true, false, 32},
    });
}

void testSchemaConstructionAndIdentifiers() {
    const auto schema = makeSchema();
    minidb::test::require(schema.columnCount() == 4, "Schema lost columns");
    minidb::test::require(schema.column(0).name == "id"
                              && schema.column(1).name == "balance"
                              && schema.column(3).name == "display_name",
                          "Schema did not normalize identifiers");
    minidb::test::require(schema.findColumn("DISPLAY_NAME") == 3
                              && schema.primaryKeyColumn() == 0,
                          "Schema lookup or primary-key discovery failed");

    minidb::test::requireThrows<std::invalid_argument>(
        [] { static_cast<void>(minidb::normalizeIdentifier("9users")); },
        "Identifier beginning with a digit was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] { static_cast<void>(minidb::normalizeIdentifier("user-name")); },
        "Identifier punctuation was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] {
            static_cast<void>(minidb::Schema::create({
                {"Name", minidb::DataType::UINT32, false, false, 0},
                {"name", minidb::DataType::INT64, false, false, 0},
            }));
        },
        "Duplicate normalized column names were accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] {
            static_cast<void>(minidb::Schema::create({
                {"a", minidb::DataType::UINT32, false, true, 0},
                {"b", minidb::DataType::UINT32, false, true, 0},
            }));
        },
        "Multiple primary keys were accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] {
            static_cast<void>(minidb::Schema::create({
                {"id", minidb::DataType::UINT32, true, true, 0},
            }));
        },
        "Nullable primary key was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] {
            static_cast<void>(minidb::Schema::create({
                {"id", minidb::DataType::INT64, false, true, 0},
            }));
        },
        "Non-UINT32 primary key was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] {
            static_cast<void>(minidb::Schema::create({
                {"text", minidb::DataType::VARCHAR, false, false, 0},
            }));
        },
        "Zero-length VARCHAR declaration was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] {
            static_cast<void>(minidb::Schema::create({
                {"number", minidb::DataType::UINT32, false, false, 4},
            }));
        },
        "VARCHAR limit on a non-VARCHAR column was accepted");
}

void testCanonicalTupleRoundTripsAndLayout() {
    const auto schema = makeSchema();
    const minidb::RowValues values{
        std::uint32_t{0x78563412U},
        std::int64_t{-2},
        true,
        std::monostate{},
    };
    const auto encoded = minidb::TupleCodec::encode(schema, values);
    using namespace minidb::tuple_encoding_layout;
    minidb::test::require(
        std::equal(MAGIC.begin(), MAGIC.end(), encoded.begin()),
        "Tuple magic was not encoded");
    minidb::test::require(minidb::byte_codec::readUint32(encoded, VERSION_OFFSET) == 1
                              && minidb::byte_codec::readUint32(encoded, HEADER_SIZE_OFFSET) == 24
                              && minidb::byte_codec::readUint16(encoded, COLUMN_COUNT_OFFSET) == 4
                              && minidb::byte_codec::readUint16(encoded, NULL_BITMAP_SIZE_OFFSET) == 1
                              && minidb::byte_codec::readUint32(encoded, ENCODED_SIZE_OFFSET)
                                  == encoded.size(),
                          "Tuple header fields were not encoded canonically");
    minidb::test::require(encoded[HEADER_SIZE] == std::byte{0x08},
                          "Tuple null bitmap did not mark the fourth column");
    minidb::test::require(encoded[HEADER_SIZE + 1] == std::byte{0x12}
                              && encoded[HEADER_SIZE + 2] == std::byte{0x34}
                              && encoded[HEADER_SIZE + 3] == std::byte{0x56}
                              && encoded[HEADER_SIZE + 4] == std::byte{0x78},
                          "UINT32 was not encoded little-endian");
    minidb::test::require(minidb::TupleCodec::decode(schema, encoded) == values,
                          "Canonical tuple did not round trip");

    const auto integerSchema = minidb::Schema::create({
        {"value", minidb::DataType::INT64, false, false, 0},
    });
    for (const auto value : {
             std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(),
             std::numeric_limits<std::int64_t>::max()}) {
        const minidb::RowValues row{value};
        minidb::test::require(
            minidb::TupleCodec::decode(integerSchema,
                                       minidb::TupleCodec::encode(integerSchema, row)) == row,
            "INT64 boundary did not round trip");
    }

    const auto boolSchema = minidb::Schema::create({
        {"flag", minidb::DataType::BOOLEAN, false, false, 0},
    });
    for (const bool value : {false, true}) {
        const minidb::RowValues row{value};
        minidb::test::require(
            minidb::TupleCodec::decode(boolSchema,
                                       minidb::TupleCodec::encode(boolSchema, row)) == row,
            "BOOLEAN did not round trip");
    }
}

void testNullBitmapVarcharAndValueValidation() {
    const auto schema = minidb::Schema::create({
        {"a", minidb::DataType::VARCHAR, true, false, 8},
        {"b", minidb::DataType::UINT32, true, false, 0},
        {"c", minidb::DataType::BOOLEAN, true, false, 0},
        {"d", minidb::DataType::INT64, true, false, 0},
        {"e", minidb::DataType::VARCHAR, true, false, 8},
        {"f", minidb::DataType::UINT32, true, false, 0},
        {"g", minidb::DataType::BOOLEAN, true, false, 0},
        {"h", minidb::DataType::INT64, true, false, 0},
        {"i", minidb::DataType::VARCHAR, true, false, 8},
    });
    const minidb::RowValues values{
        std::string{}, std::monostate{}, true, std::monostate{}, std::string("12345678"),
        std::uint32_t{9}, false, std::int64_t{-9}, std::monostate{},
    };
    const auto encoded = minidb::TupleCodec::encode(schema, values);
    minidb::test::require(
        minidb::byte_codec::readUint16(
            encoded, minidb::tuple_encoding_layout::NULL_BITMAP_SIZE_OFFSET) == 2
            && encoded[minidb::tuple_encoding_layout::HEADER_SIZE] == std::byte{0x0A}
            && encoded[minidb::tuple_encoding_layout::HEADER_SIZE + 1] == std::byte{0x01},
        "Multi-byte null bitmap was encoded incorrectly");
    minidb::test::require(minidb::TupleCodec::decode(schema, encoded) == values,
                          "Nullable/VARCHAR tuple did not round trip");

    const auto required = minidb::Schema::create({
        {"name", minidb::DataType::VARCHAR, false, false, 4},
    });
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(minidb::TupleCodec::encode(required, {std::monostate{}})); },
        "NULL in non-nullable column was encoded");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(minidb::TupleCodec::encode(required, {std::string("12345")})); },
        "Oversized VARCHAR was encoded");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(minidb::TupleCodec::encode(required, {std::uint32_t{1}})); },
        "Wrong logical type was encoded");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(minidb::TupleCodec::encode(required, {})); },
        "Wrong value count was encoded");

    const auto largeSchema = minidb::Schema::create({
        {"a", minidb::DataType::VARCHAR, false, false, 4000},
        {"b", minidb::DataType::VARCHAR, false, false, 4000},
    });
    minidb::test::requireThrows<std::invalid_argument>(
        [&] {
            static_cast<void>(minidb::TupleCodec::encode(
                largeSchema, {std::string(2500, 'a'), std::string(2500, 'b')}));
        },
        "Tuple larger than TupleStore inline capacity was encoded");
}

void testTupleCorruptionRejection() {
    const auto schema = minidb::Schema::create({
        {"flag", minidb::DataType::BOOLEAN, false, false, 0},
        {"text", minidb::DataType::VARCHAR, true, false, 10},
    });
    const minidb::RowValues values{true, std::string("abc")};
    const auto original = minidb::TupleCodec::encode(schema, values);
    const auto reject = [&](minidb::TupleBytes bytes, std::string_view message) {
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(minidb::TupleCodec::decode(schema, bytes)); }, message);
    };
    auto bytes = original;
    bytes[0] = std::byte{'X'};
    reject(bytes, "Bad tuple magic was accepted");
    bytes = original;
    minidb::byte_codec::writeUint32(
        bytes, minidb::tuple_encoding_layout::VERSION_OFFSET, 2);
    reject(bytes, "Unsupported tuple version was accepted");
    bytes = original;
    bytes[minidb::tuple_encoding_layout::HEADER_SIZE] = std::byte{0x80};
    reject(bytes, "Nonzero unused null-bitmap bits were accepted");
    bytes = original;
    bytes[minidb::tuple_encoding_layout::HEADER_SIZE] = std::byte{0x02};
    reject(bytes, "Trailing bytes caused by corrupt null bitmap were accepted");
    bytes = original;
    bytes[minidb::tuple_encoding_layout::HEADER_SIZE + 1] = std::byte{2};
    reject(bytes, "Invalid BOOLEAN byte was accepted");
    bytes = original;
    minidb::byte_codec::writeUint32(
        bytes, minidb::tuple_encoding_layout::HEADER_SIZE + 2, 100);
    reject(bytes, "Malformed VARCHAR length was accepted");
    bytes = original;
    bytes.pop_back();
    minidb::byte_codec::writeUint32(
        bytes, minidb::tuple_encoding_layout::ENCODED_SIZE_OFFSET,
        static_cast<std::uint32_t>(bytes.size()));
    reject(bytes, "Truncated VARCHAR payload was accepted");
    bytes = original;
    bytes.push_back(std::byte{0});
    minidb::byte_codec::writeUint32(
        bytes, minidb::tuple_encoding_layout::ENCODED_SIZE_OFFSET,
        static_cast<std::uint32_t>(bytes.size()));
    reject(bytes, "Trailing tuple garbage was accepted");
}

void testSchemaEncodingAndCorruption() {
    const auto schema = makeSchema();
    const auto encoded = minidb::encodeSchema(schema);
    using namespace minidb::schema_encoding_layout;
    minidb::test::require(
        std::equal(std::begin(MAGIC), std::end(MAGIC), encoded.begin())
            && minidb::byte_codec::readUint32(encoded, VERSION_OFFSET) == 1
            && minidb::byte_codec::readUint32(encoded, HEADER_SIZE_OFFSET) == 24
            && minidb::byte_codec::readUint16(encoded, COLUMN_COUNT_OFFSET) == 4
            && minidb::byte_codec::readUint32(encoded, ENCODED_SIZE_OFFSET) == encoded.size(),
        "Schema header binary layout was incorrect");
    minidb::test::require(minidb::decodeSchema(encoded) == schema,
                          "Schema encoding did not round trip");

    const auto reject = [&](minidb::TupleBytes bytes, std::string_view message) {
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(minidb::decodeSchema(bytes)); }, message);
    };
    auto bytes = encoded;
    bytes[0] = std::byte{'X'};
    reject(bytes, "Bad schema magic was accepted");
    bytes = encoded;
    minidb::byte_codec::writeUint32(bytes, VERSION_OFFSET, 2);
    reject(bytes, "Unsupported schema version was accepted");
    bytes = encoded;
    bytes[HEADER_SIZE + COLUMN_TYPE_OFFSET] = std::byte{99};
    reject(bytes, "Unknown schema type ID was accepted");
    bytes = encoded;
    bytes[HEADER_SIZE + COLUMN_FLAGS_OFFSET] = std::byte{0x80};
    reject(bytes, "Unknown schema flags were accepted");
    bytes = encoded;
    minidb::byte_codec::writeUint32(
        bytes, HEADER_SIZE + COLUMN_VARCHAR_MAX_OFFSET, 1);
    reject(bytes, "VARCHAR maximum on fixed type was accepted");
    bytes = encoded;
    bytes.pop_back();
    minidb::byte_codec::writeUint32(
        bytes, ENCODED_SIZE_OFFSET, static_cast<std::uint32_t>(bytes.size()));
    reject(bytes, "Truncated schema was accepted");
}

} // namespace

int main() {
    try {
        testSchemaConstructionAndIdentifiers();
        testCanonicalTupleRoundTripsAndLayout();
        testNullBitmapVarcharAndValueValidation();
        testTupleCorruptionRejection();
        testSchemaEncodingAndCorruption();
        std::cout << "schema_tuple_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "schema_tuple_test failed: " << error.what() << '\n';
        return 1;
    }
}
