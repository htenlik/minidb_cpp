#pragma once

#include "minidb/sql_source.hpp"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace minidb::sql {

struct NullLiteral {
    bool operator==(const NullLiteral&) const = default;
};

struct BooleanLiteral {
    bool value = false;
    bool operator==(const BooleanLiteral&) const = default;
};

struct IntegerLiteral {
    bool negative = false;
    std::string magnitude;
    bool operator==(const IntegerLiteral&) const = default;
};

struct StringLiteral {
    std::string value;
    bool operator==(const StringLiteral&) const = default;
};

using LiteralValue = std::variant<
    NullLiteral,
    BooleanLiteral,
    IntegerLiteral,
    StringLiteral>;

struct SqlLiteral {
    SourceSpan span{};
    LiteralValue value;

    bool operator==(const SqlLiteral&) const = default;
};

enum class UnaryOperator {
    Not,
};

enum class BinaryOperator {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
};

struct Expression;

struct IdentifierExpression {
    std::string name;
};

struct LiteralExpression {
    SqlLiteral literal;
};

struct UnaryExpression {
    UnaryOperator op;
    std::unique_ptr<Expression> operand;
};

struct BinaryExpression {
    BinaryOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
};

using ExpressionNode = std::variant<
    IdentifierExpression,
    LiteralExpression,
    UnaryExpression,
    BinaryExpression>;

struct Expression {
    SourceSpan span{};
    ExpressionNode node;
};

enum class SqlTypeKind {
    Uint32,
    Int64,
    Boolean,
    Varchar,
};

struct SqlTypeSpecification {
    SqlTypeKind kind = SqlTypeKind::Uint32;
    std::optional<std::string> varcharSizeMagnitude;
    SourceSpan span{};
};

enum class NullConstraint {
    Unspecified,
    Null,
    NotNull,
};

struct ColumnSpecification {
    std::string name;
    SqlTypeSpecification type;
    NullConstraint nullConstraint = NullConstraint::Unspecified;
    bool primaryKey = false;
    SourceSpan span{};
};

struct CreateTableStatement {
    std::string tableName;
    std::vector<ColumnSpecification> columns;
};

struct InsertStatement {
    std::string tableName;
    std::optional<std::vector<std::string>> columns;
    std::vector<SqlLiteral> values;
};

struct SelectStatement {
    bool selectAll = false;
    std::vector<std::string> columns;
    std::string tableName;
    std::unique_ptr<Expression> where;
};

struct Assignment {
    std::string columnName;
    SqlLiteral value;
    SourceSpan span{};
};

struct UpdateStatement {
    std::string tableName;
    std::vector<Assignment> assignments;
    std::unique_ptr<Expression> where;
};

struct DeleteStatement {
    std::string tableName;
    std::unique_ptr<Expression> where;
};

using StatementNode = std::variant<
    CreateTableStatement,
    InsertStatement,
    SelectStatement,
    UpdateStatement,
    DeleteStatement>;

struct Statement {
    SourceSpan span{};
    StatementNode node;
};

[[nodiscard]] std::string toDebugString(const SqlLiteral& literal);
[[nodiscard]] std::string toDebugString(const Expression& expression);
[[nodiscard]] std::string toDebugString(const Statement& statement);

} // namespace minidb::sql
