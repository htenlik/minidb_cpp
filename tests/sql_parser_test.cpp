#include "minidb/sql_ast.hpp"
#include "minidb/sql_error.hpp"
#include "minidb/sql_parser.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using minidb::sql::BinaryExpression;
using minidb::sql::BinaryOperator;
using minidb::sql::BooleanLiteral;
using minidb::sql::ColumnSpecification;
using minidb::sql::CreateTableStatement;
using minidb::sql::DeleteStatement;
using minidb::sql::InsertStatement;
using minidb::sql::IntegerLiteral;
using minidb::sql::NullConstraint;
using minidb::sql::NullLiteral;
using minidb::sql::Parser;
using minidb::sql::SelectStatement;
using minidb::sql::SqlError;
using minidb::sql::SqlErrorKind;
using minidb::sql::SqlTypeKind;
using minidb::sql::Statement;
using minidb::sql::StringLiteral;
using minidb::sql::UnaryExpression;
using minidb::sql::UpdateStatement;

Statement parse(std::string_view source) {
    return Parser::parse(source);
}

template <typename Node>
const Node& requireStatement(const Statement& statement, std::string_view message) {
    minidb::test::require(std::holds_alternative<Node>(statement.node), message);
    return std::get<Node>(statement.node);
}

void requireParserError(
    std::string_view source,
    std::string_view expectedMessage = {}) {
    try {
        static_cast<void>(parse(source));
    } catch (const SqlError& error) {
        minidb::test::require(error.kind() == SqlErrorKind::Parser,
                              "Malformed grammar produced a lexer error");
        if (!expectedMessage.empty()) {
            minidb::test::require(error.message().find(expectedMessage) != std::string::npos,
                                  "Parser error lacked expected diagnostic text");
        }
        return;
    }
    throw std::runtime_error("Malformed SQL parsed successfully: " + std::string(source));
}

void testCreateTableAst() {
    const auto statement = parse(
        "CrEaTe TABLE Users (\n"
        " id UINT32 NOT NULL PRIMARY KEY,\n"
        " username VARCHAR(32) NULL,\n"
        " score INT64, active BOOLEAN PRIMARY KEY NOT NULL\n"
        ");");
    const auto& create = requireStatement<CreateTableStatement>(
        statement, "CREATE TABLE produced wrong statement kind");
    minidb::test::require(create.tableName == "Users" && create.columns.size() == 4,
                          "CREATE TABLE did not preserve name/columns");
    const auto& id = create.columns[0];
    minidb::test::require(id.name == "id" && id.type.kind == SqlTypeKind::Uint32
                              && id.primaryKey
                              && id.nullConstraint == NullConstraint::NotNull,
                          "UINT32 constraints were not represented correctly");
    const auto& username = create.columns[1];
    minidb::test::require(username.type.kind == SqlTypeKind::Varchar
                              && username.type.varcharSizeMagnitude == "32"
                              && username.nullConstraint == NullConstraint::Null,
                          "VARCHAR syntax was not preserved");
    minidb::test::require(create.columns[2].type.kind == SqlTypeKind::Int64
                              && create.columns[3].type.kind == SqlTypeKind::Boolean,
                          "CREATE TABLE supported types were parsed incorrectly");
    minidb::test::require(statement.span.begin.line == 1
                              && statement.span.begin.column == 1
                              && statement.span.end.line == 5,
                          "CREATE statement source span was incorrect");
    minidb::test::require(create.columns[1].span.begin.line == 3,
                          "Column AST did not retain its source location");

    const auto semanticInvalid = parse(
        "CREATE TABLE t (id VARCHAR(00020) PRIMARY KEY, id UINT32);");
    const auto& syntax = std::get<CreateTableStatement>(semanticInvalid.node);
    minidb::test::require(syntax.columns[0].type.varcharSizeMagnitude == "00020"
                              && syntax.columns[0].primaryKey
                              && syntax.columns[0].name == syntax.columns[1].name,
                          "Parser performed forbidden Schema semantic validation");
    minidb::test::require(
        minidb::sql::toDebugString(statement)
            == "Create(table=Users, columns=[id:UINT32:PK:NOT_NULL,"
               "username:VARCHAR(32):NULL,score:INT64,active:BOOLEAN:PK:NOT_NULL])",
        "CREATE debug AST representation was not deterministic");
}

void testInsertAstAndLiteralBoundaries() {
    const auto implicit = parse(
        "INSERT INTO users VALUES "
        "(0, 4294967295, 9223372036854775807, -9223372036854775808, "
        "999999999999999999999999999999, '', 'it''s', TRUE, FALSE, NULL)");
    const auto& insert = requireStatement<InsertStatement>(
        implicit, "INSERT produced wrong statement kind");
    minidb::test::require(insert.tableName == "users" && !insert.columns.has_value()
                              && insert.values.size() == 10,
                          "Implicit INSERT AST was incorrect");
    const auto& uintMax = std::get<IntegerLiteral>(insert.values[1].value);
    const auto& intMin = std::get<IntegerLiteral>(insert.values[3].value);
    const auto& beyond = std::get<IntegerLiteral>(insert.values[4].value);
    minidb::test::require(!uintMax.negative && uintMax.magnitude == "4294967295"
                              && intMin.negative
                              && intMin.magnitude == "9223372036854775808"
                              && beyond.magnitude == "999999999999999999999999999999",
                          "Integer AST did not preserve sign/magnitude losslessly");
    minidb::test::require(std::get<StringLiteral>(insert.values[5].value).value.empty()
                              && std::get<StringLiteral>(insert.values[6].value).value == "it's"
                              && std::get<BooleanLiteral>(insert.values[7].value).value
                              && !std::get<BooleanLiteral>(insert.values[8].value).value
                              && std::holds_alternative<NullLiteral>(insert.values[9].value),
                          "INSERT literal AST values were incorrect");

    const auto explicitColumns = parse(
        "INSERT INTO UsErS (id, userName, id) VALUES (1, 'abc', NULL);");
    const auto& explicitInsert = std::get<InsertStatement>(explicitColumns.node);
    minidb::test::require(explicitInsert.columns == std::optional<std::vector<std::string>>{
                              {"id", "userName", "id"}},
                          "Explicit INSERT columns were not preserved in source spelling/order");
    minidb::test::require(
        minidb::sql::toDebugString(explicitColumns)
            == "Insert(table=UsErS, columns=[id,userName,id], "
               "values=[Int(1),String(\"abc\"),Null])",
        "INSERT debug AST representation was incorrect");
}

void testSelectProjectionAndExpressionPrecedence() {
    const auto all = parse("SELECT * FROM users;");
    const auto& allSelect = requireStatement<SelectStatement>(
        all, "SELECT produced wrong statement kind");
    minidb::test::require(allSelect.selectAll && allSelect.columns.empty()
                              && allSelect.tableName == "users" && !allSelect.where,
                          "SELECT * AST was incorrect");

    const auto selected = parse(
        "SELECT id, UserName FROM MissingTable "
        "WHERE a = 1 OR b = 2 AND c = 3");
    const auto& select = std::get<SelectStatement>(selected.node);
    minidb::test::require(!select.selectAll
                              && select.columns == std::vector<std::string>{"id", "UserName"}
                              && select.tableName == "MissingTable",
                          "Named SELECT projection was incorrect");
    minidb::test::require(
        minidb::sql::toDebugString(*select.where)
            == "Or(Eq(Id(a),Int(1)),And(Eq(Id(b),Int(2)),Eq(Id(c),Int(3))))",
        "AND/OR precedence was incorrect");

    const auto grouped = parse(
        "SELECT * FROM t WHERE (a = 1 OR b = 2) AND NOT c = FALSE");
    minidb::test::require(
        minidb::sql::toDebugString(*std::get<SelectStatement>(grouped.node).where)
            == "And(Or(Eq(Id(a),Int(1)),Eq(Id(b),Int(2))),"
               "Not(Eq(Id(c),Bool(false))))",
        "Parentheses or NOT precedence was incorrect");

    const std::vector<std::pair<std::string, std::string>> comparisons{
        {"=", "Eq"}, {"!=", "Ne"}, {"<>", "Ne"}, {"<", "Lt"},
        {"<=", "Le"}, {">", "Gt"}, {">=", "Ge"},
    };
    for (const auto& [spelling, debugName] : comparisons) {
        const auto statement = parse("SELECT * FROM t WHERE value " + spelling + " -100");
        const auto& expression = *std::get<SelectStatement>(statement.node).where;
        minidb::test::require(
            minidb::sql::toDebugString(expression)
                == debugName + "(Id(value),Int(-100))",
            "Comparison operator AST was incorrect");
    }
}

void testUpdateDeleteAndOptionalTerminator() {
    const auto updateStatement = parse(
        "UPDATE Users SET name = 'new', email = NULL, active = FALSE "
        "WHERE id >= 10 AND id <= 20;");
    const auto& update = requireStatement<UpdateStatement>(
        updateStatement, "UPDATE produced wrong statement kind");
    minidb::test::require(update.tableName == "Users" && update.assignments.size() == 3
                              && update.assignments[0].columnName == "name"
                              && std::get<StringLiteral>(update.assignments[0].value.value).value
                                  == "new"
                              && std::holds_alternative<NullLiteral>(
                                  update.assignments[1].value.value),
                          "UPDATE assignment AST was incorrect");
    minidb::test::require(
        minidb::sql::toDebugString(*update.where)
            == "And(Ge(Id(id),Int(10)),Le(Id(id),Int(20)))",
        "UPDATE WHERE AST was incorrect");

    const auto withoutWhere = parse("UPDATE t SET active = FALSE");
    minidb::test::require(!std::get<UpdateStatement>(withoutWhere.node).where,
                          "Optional UPDATE WHERE was not optional");

    const auto deletion = parse("DELETE FROM users WHERE id = 42;");
    const auto& deleteNode = requireStatement<DeleteStatement>(
        deletion, "DELETE produced wrong statement kind");
    minidb::test::require(deleteNode.tableName == "users"
                              && minidb::sql::toDebugString(*deleteNode.where)
                                  == "Eq(Id(id),Int(42))",
                          "DELETE AST was incorrect");
    const auto deleteAll = parse("DELETE FROM users");
    minidb::test::require(!std::get<DeleteStatement>(deleteAll.node).where,
                          "Optional DELETE WHERE was not optional");
}

void testMalformedStatementFamilies() {
    const std::vector<std::string_view> malformed{
        "",
        "CREATE users (id UINT32)",
        "CREATE TABLE (id UINT32)",
        "CREATE TABLE t id UINT32)",
        "CREATE TABLE t ()",
        "CREATE TABLE t (id)",
        "CREATE TABLE t (id VARCHAR)",
        "CREATE TABLE t (id VARCHAR())",
        "CREATE TABLE t (id VARCHAR(12)",
        "CREATE TABLE t (id UINT32 name INT64)",
        "CREATE TABLE t (id UINT32,)",
        "CREATE TABLE t (id UINT32 NULL NOT NULL)",
        "CREATE TABLE t (id UINT32 PRIMARY KEY PRIMARY KEY)",
        "INSERT t VALUES (1)",
        "INSERT INTO VALUES (1)",
        "INSERT INTO t (id) (1)",
        "INSERT INTO t () VALUES (1)",
        "INSERT INTO t (id,,name) VALUES (1,2,3)",
        "INSERT INTO t VALUES ()",
        "INSERT INTO t VALUES (1,)",
        "INSERT INTO t VALUES (id)",
        "INSERT INTO t VALUES (1 = 2)",
        "SELECT FROM t",
        "SELECT *, id FROM t",
        "SELECT id, FROM t",
        "SELECT id t",
        "SELECT * FROM",
        "SELECT * FROM t WHERE",
        "SELECT * FROM t WHERE a = b = c",
        "UPDATE SET a = 1",
        "UPDATE t a = 1",
        "UPDATE t SET",
        "UPDATE t SET a 1",
        "UPDATE t SET a =",
        "UPDATE t SET a = 1,, b = 2",
        "UPDATE t SET a = b",
        "UPDATE t SET a = 1 WHERE )",
        "DELETE t",
        "DELETE FROM",
        "DELETE FROM t WHERE",
        "SELECT * FROM t; garbage",
        "SELECT * FROM t; SELECT * FROM u",
    };
    for (const auto source : malformed) {
        requireParserError(source);
    }
    requireParserError("SELECT * FROM t WHERE a = b = c", "chained comparisons");
    requireParserError("UPDATE t SET;", "at least one assignment");
    requireParserError("CREATE TABLE t (id UINT32,)", "trailing comma");
}

void testErrorLocationsAndNestingLimit() {
    try {
        static_cast<void>(parse("SELECT id\nFROM t\nWHERE )"));
        throw std::runtime_error("Expected multiline parser error");
    } catch (const SqlError& error) {
        minidb::test::require(error.kind() == SqlErrorKind::Parser
                                  && error.span().begin.line == 3
                                  && error.span().begin.column == 7,
                              "Parser error location was not line 3, column 7");
    }

    std::string maximum = "SELECT * FROM t WHERE ";
    maximum.append(Parser::MAX_EXPRESSION_NESTING, '(');
    maximum += "id = 1";
    maximum.append(Parser::MAX_EXPRESSION_NESTING, ')');
    static_cast<void>(parse(maximum));

    std::string tooDeep = "SELECT * FROM t WHERE ";
    tooDeep.append(Parser::MAX_EXPRESSION_NESTING + 1, '(');
    tooDeep += "id = 1";
    tooDeep.append(Parser::MAX_EXPRESSION_NESTING + 1, ')');
    requireParserError(tooDeep, "nesting exceeds");

    std::string notTooDeep = "SELECT * FROM t WHERE ";
    for (std::size_t index = 0; index < Parser::MAX_EXPRESSION_NESTING + 1; ++index) {
        notTooDeep += "NOT ";
    }
    notTooDeep += "id = 1";
    requireParserError(notTooDeep, "nesting exceeds");
}

void testLargeExpressionAndAstMove() {
    std::string source = "SELECT * FROM t WHERE id = 0";
    for (std::size_t index = 1; index < 500; ++index) {
        source += " OR id = " + std::to_string(index);
    }
    auto statement = parse(source);
    Statement moved = std::move(statement);
    const auto& select = std::get<SelectStatement>(moved.node);
    minidb::test::require(select.where != nullptr
                              && select.where->span.begin.column == 23,
                          "Large expression AST move/source span failed");
}

} // namespace

int main() {
    try {
        testCreateTableAst();
        testInsertAstAndLiteralBoundaries();
        testSelectProjectionAndExpressionPrecedence();
        testUpdateDeleteAndOptionalTerminator();
        testMalformedStatementFamilies();
        testErrorLocationsAndNestingLimit();
        testLargeExpressionAndAstMove();
        std::cout << "sql_parser_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_parser_test failed: " << error.what() << '\n';
        return 1;
    }
}
