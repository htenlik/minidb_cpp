#include "minidb/sql_parser.hpp"
#include "minidb/sql_semantics.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

using minidb::ColumnDefinition;
using minidb::DataType;
using minidb::Value;
using minidb::sql::InsertStatement;
using minidb::sql::Parser;
using minidb::sql::SqlExecutionError;
using minidb::sql::SqlExecutionErrorKind;
using minidb::sql::SqlLiteral;
using minidb::sql::TruthValue;

SqlLiteral parseLiteral(std::string_view spelling) {
    auto statement = Parser::parse("INSERT INTO t VALUES (" + std::string(spelling) + ")");
    auto& insert = std::get<InsertStatement>(statement.node);
    return std::move(insert.values.front());
}

template <typename Expected>
void requireValue(
    std::string_view spelling,
    const ColumnDefinition& column,
    Expected expected) {
    const Value value = minidb::sql::convertLiteral(parseLiteral(spelling), column);
    minidb::test::require(std::holds_alternative<Expected>(value)
                              && std::get<Expected>(value) == expected,
                          "Converted SQL literal did not match its target value");
}

void requireConversionError(
    std::string_view spelling,
    const ColumnDefinition& column,
    SqlExecutionErrorKind kind) {
    try {
        static_cast<void>(minidb::sql::convertLiteral(parseLiteral(spelling), column));
    } catch (const SqlExecutionError& error) {
        minidb::test::require(error.kind() == kind
                                  && error.span().begin.line == 1
                                  && error.span().begin.column == 23,
                              "Literal conversion error kind/span was incorrect");
        return;
    }
    throw std::runtime_error("Invalid SQL literal conversion succeeded");
}

void testIntegerBoundaries() {
    const ColumnDefinition uintColumn{"id", DataType::UINT32, false, false, 0};
    requireValue<std::uint32_t>("0", uintColumn, 0);
    requireValue<std::uint32_t>(
        "4294967295", uintColumn, std::numeric_limits<std::uint32_t>::max());
    requireConversionError("-1", uintColumn, SqlExecutionErrorKind::Constraint);
    requireConversionError("4294967296", uintColumn, SqlExecutionErrorKind::Constraint);

    const ColumnDefinition intColumn{"score", DataType::INT64, false, false, 0};
    requireValue<std::int64_t>("0", intColumn, 0);
    requireValue<std::int64_t>("-1", intColumn, -1);
    requireValue<std::int64_t>(
        "9223372036854775807", intColumn, std::numeric_limits<std::int64_t>::max());
    requireValue<std::int64_t>(
        "-9223372036854775808", intColumn, std::numeric_limits<std::int64_t>::min());
    requireConversionError(
        "9223372036854775808", intColumn, SqlExecutionErrorKind::Constraint);
    requireConversionError(
        "-9223372036854775809", intColumn, SqlExecutionErrorKind::Constraint);
    requireConversionError(
        std::string(1024, '9'), intColumn, SqlExecutionErrorKind::Constraint);
}

void testStrictTypesNullabilityAndVarchar() {
    const ColumnDefinition booleanColumn{"active", DataType::BOOLEAN, false, false, 0};
    const ColumnDefinition stringColumn{"name", DataType::VARCHAR, true, false, 3};
    requireValue<bool>("TRUE", booleanColumn, true);
    requireValue<std::string>("'abc'", stringColumn, "abc");
    const auto nullValue = minidb::sql::convertLiteral(parseLiteral("NULL"), stringColumn);
    minidb::test::require(std::holds_alternative<std::monostate>(nullValue),
                          "Nullable SQL NULL did not convert to monostate");
    requireConversionError("NULL", booleanColumn, SqlExecutionErrorKind::Constraint);
    requireConversionError("1", booleanColumn, SqlExecutionErrorKind::Semantic);
    requireConversionError("TRUE", stringColumn, SqlExecutionErrorKind::Semantic);
    requireConversionError("'abcd'", stringColumn, SqlExecutionErrorKind::Constraint);
}

void testTruthTables() {
    using minidb::sql::truthAnd;
    using minidb::sql::truthNot;
    using minidb::sql::truthOr;
    minidb::test::require(
        truthNot(TruthValue::True) == TruthValue::False
            && truthNot(TruthValue::False) == TruthValue::True
            && truthNot(TruthValue::Unknown) == TruthValue::Unknown,
        "SQL NOT truth table was incorrect");

    constexpr TruthValue values[]{
        TruthValue::False, TruthValue::True, TruthValue::Unknown,
    };
    constexpr TruthValue expectedAnd[3][3]{
        {TruthValue::False, TruthValue::False, TruthValue::False},
        {TruthValue::False, TruthValue::True, TruthValue::Unknown},
        {TruthValue::False, TruthValue::Unknown, TruthValue::Unknown},
    };
    constexpr TruthValue expectedOr[3][3]{
        {TruthValue::False, TruthValue::True, TruthValue::Unknown},
        {TruthValue::True, TruthValue::True, TruthValue::True},
        {TruthValue::Unknown, TruthValue::True, TruthValue::Unknown},
    };
    for (std::size_t left = 0; left < 3; ++left) {
        for (std::size_t right = 0; right < 3; ++right) {
            minidb::test::require(
                truthAnd(values[left], values[right]) == expectedAnd[left][right]
                    && truthOr(values[left], values[right]) == expectedOr[left][right],
                "SQL AND/OR truth table was incorrect");
        }
    }
}

} // namespace

int main() {
    try {
        testIntegerBoundaries();
        testStrictTypesNullabilityAndVarchar();
        testTruthTables();
        std::cout << "sql_semantics_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_semantics_test failed: " << error.what() << '\n';
        return 1;
    }
}
