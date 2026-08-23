#pragma once

#include "minidb/tuple_bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb {

inline constexpr std::size_t MAX_IDENTIFIER_BYTES = 63;
inline constexpr std::size_t MAX_SCHEMA_COLUMNS = 1024;
inline constexpr std::uint32_t MAX_VARCHAR_BYTES = 4000;

enum class DataType : std::uint8_t {
    UINT32 = 1,
    INT64 = 2,
    BOOLEAN = 3,
    VARCHAR = 4,
};

using Value = std::variant<
    std::monostate,
    std::uint32_t,
    std::int64_t,
    bool,
    std::string>;
using RowValues = std::vector<Value>;

struct ColumnDefinition {
    std::string name;
    DataType type = DataType::UINT32;
    bool nullable = false;
    bool primaryKey = false;
    std::uint32_t varcharMaxBytes = 0;

    bool operator==(const ColumnDefinition&) const = default;
};

[[nodiscard]] std::string normalizeIdentifier(std::string_view identifier);

class Schema {
public:
    [[nodiscard]] static Schema create(std::vector<ColumnDefinition> columns);

    [[nodiscard]] std::size_t columnCount() const noexcept { return columns_.size(); }
    [[nodiscard]] const ColumnDefinition& column(std::size_t index) const;
    [[nodiscard]] const std::vector<ColumnDefinition>& columns() const noexcept {
        return columns_;
    }
    [[nodiscard]] std::optional<std::size_t> findColumn(std::string_view name) const;
    [[nodiscard]] std::optional<std::size_t> primaryKeyColumn() const noexcept;
    void validateValues(const RowValues& values) const;

    bool operator==(const Schema&) const = default;

private:
    explicit Schema(std::vector<ColumnDefinition> columns)
        : columns_(std::move(columns)) {}

    std::vector<ColumnDefinition> columns_;
};

namespace schema_encoding_layout {

using EncodingVersion = std::uint32_t;
inline constexpr EncodingVersion CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::byte MAGIC[MAGIC_SIZE]{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'S'},
    std::byte{'C'}, std::byte{'H'}, std::byte{'M'}, std::byte{'A'},
};
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t COLUMN_COUNT_OFFSET = 16;
inline constexpr std::size_t FLAGS_OFFSET = 18;
inline constexpr std::size_t ENCODED_SIZE_OFFSET = 20;
inline constexpr std::size_t HEADER_SIZE = 24;

inline constexpr std::size_t COLUMN_NAME_LENGTH_OFFSET = 0;
inline constexpr std::size_t COLUMN_TYPE_OFFSET = 2;
inline constexpr std::size_t COLUMN_FLAGS_OFFSET = 3;
inline constexpr std::size_t COLUMN_VARCHAR_MAX_OFFSET = 4;
inline constexpr std::size_t COLUMN_HEADER_SIZE = 8;
inline constexpr std::uint8_t NULLABLE_FLAG = 0x01;
inline constexpr std::uint8_t PRIMARY_KEY_FLAG = 0x02;

} // namespace schema_encoding_layout

[[nodiscard]] TupleBytes encodeSchema(const Schema& schema);
[[nodiscard]] Schema decodeSchema(std::span<const std::byte> bytes);

} // namespace minidb
