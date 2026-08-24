#include "minidb/sql_executor.hpp"

#include "minidb/sql_parser.hpp"
#include "minidb/table.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace minidb::sql {
namespace {

enum class BoundType {
    Null,
    Uint32,
    Int64,
    Boolean,
    Varchar,
    Integer,
};

using BoundScalar = std::variant<
    std::monostate,
    std::uint32_t,
    std::int64_t,
    bool,
    std::string,
    IntegerLiteral>;

struct BoundColumn {
    std::size_t index;
};

struct BoundLiteral {
    BoundScalar value;
};

struct BoundExpression;

struct BoundUnary {
    std::unique_ptr<BoundExpression> operand;
};

struct BoundBinary {
    BinaryOperator op;
    std::unique_ptr<BoundExpression> left;
    std::unique_ptr<BoundExpression> right;
};

using BoundNode = std::variant<BoundColumn, BoundLiteral, BoundUnary, BoundBinary>;

struct BoundExpression {
    SourceSpan span{};
    BoundType type = BoundType::Null;
    BoundNode node;
};

struct BoundAssignment {
    std::size_t columnIndex;
    Value value;
    SourceSpan span;
};

[[noreturn]] void fail(
    SqlExecutionErrorKind kind,
    std::string message,
    SourceSpan span) {
    throw SqlExecutionError(kind, std::move(message), span);
}

BoundType boundType(DataType type) {
    switch (type) {
    case DataType::UINT32: return BoundType::Uint32;
    case DataType::INT64: return BoundType::Int64;
    case DataType::BOOLEAN: return BoundType::Boolean;
    case DataType::VARCHAR: return BoundType::Varchar;
    }
    return BoundType::Null;
}

bool isOrdering(BinaryOperator op) noexcept {
    return op == BinaryOperator::Less || op == BinaryOperator::LessEqual
        || op == BinaryOperator::Greater || op == BinaryOperator::GreaterEqual;
}

std::size_t resolveColumn(
    const Schema& schema,
    std::string_view name,
    SourceSpan span) {
    try {
        const auto index = schema.findColumn(name);
        if (!index.has_value()) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "column '" + std::string(name) + "' does not exist",
                span);
        }
        return *index;
    } catch (const SqlExecutionError&) {
        throw;
    } catch (const std::invalid_argument& error) {
        fail(SqlExecutionErrorKind::Semantic, error.what(), span);
    }
}

Table openTable(Catalog& catalog, std::string_view name, SourceSpan span) {
    try {
        if (!catalog.findTable(name).has_value()) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "table '" + std::string(name) + "' does not exist",
                span);
        }
        return catalog.openTable(name);
    } catch (const SqlExecutionError&) {
        throw;
    } catch (const std::invalid_argument& error) {
        fail(SqlExecutionErrorKind::Semantic, error.what(), span);
    }
}

BoundType literalType(const SqlLiteral& literal) {
    if (std::holds_alternative<NullLiteral>(literal.value)) {
        return BoundType::Null;
    }
    if (std::holds_alternative<BooleanLiteral>(literal.value)) {
        return BoundType::Boolean;
    }
    if (std::holds_alternative<StringLiteral>(literal.value)) {
        return BoundType::Varchar;
    }
    return BoundType::Integer;
}

BoundScalar literalScalar(const SqlLiteral& literal) {
    return std::visit(
        [](const auto& value) -> BoundScalar {
            using ValueType = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<ValueType, NullLiteral>) {
                return std::monostate{};
            } else if constexpr (std::is_same_v<ValueType, BooleanLiteral>) {
                return value.value;
            } else if constexpr (std::is_same_v<ValueType, StringLiteral>) {
                return value.value;
            } else {
                return value;
            }
        },
        literal.value);
}

BoundScalar valueScalar(const Value& value) {
    return std::visit(
        [](const auto& item) -> BoundScalar { return item; },
        value);
}

const SqlLiteral* syntaxLiteral(const Expression& expression) {
    const auto* literal = std::get_if<LiteralExpression>(&expression.node);
    return literal == nullptr ? nullptr : &literal->literal;
}

const IdentifierExpression* syntaxIdentifier(const Expression& expression) {
    return std::get_if<IdentifierExpression>(&expression.node);
}

std::unique_ptr<BoundExpression> bindExpression(
    const Expression& expression,
    const Schema& schema);

void convertContextualLiteral(
    std::unique_ptr<BoundExpression>& literalExpression,
    const SqlLiteral& literal,
    const ColumnDefinition& column) {
    if (std::holds_alternative<NullLiteral>(literal.value)) {
        return;
    }
    auto converted = convertLiteral(literal, column);
    literalExpression->type = boundType(column.type);
    std::get<BoundLiteral>(literalExpression->node).value = valueScalar(converted);
}

void validateComparisonTypes(
    BinaryOperator op,
    std::unique_ptr<BoundExpression>& left,
    std::unique_ptr<BoundExpression>& right,
    const Expression& syntaxLeft,
    const Expression& syntaxRight,
    const Schema& schema,
    SourceSpan span) {
    if ((left->type == BoundType::Boolean || right->type == BoundType::Boolean)
        && isOrdering(op)) {
        fail(
            SqlExecutionErrorKind::Semantic,
            "BOOLEAN supports only equality and inequality comparisons",
            span);
    }
    if (left->type == BoundType::Null || right->type == BoundType::Null) {
        return;
    }

    const auto* leftIdentifier = syntaxIdentifier(syntaxLeft);
    const auto* rightIdentifier = syntaxIdentifier(syntaxRight);
    const auto* leftLiteral = syntaxLiteral(syntaxLeft);
    const auto* rightLiteral = syntaxLiteral(syntaxRight);
    if (leftIdentifier != nullptr && rightLiteral != nullptr) {
        const auto index = std::get<BoundColumn>(left->node).index;
        convertContextualLiteral(right, *rightLiteral, schema.column(index));
    } else if (leftLiteral != nullptr && rightIdentifier != nullptr) {
        const auto index = std::get<BoundColumn>(right->node).index;
        convertContextualLiteral(left, *leftLiteral, schema.column(index));
    }

    if (left->type != right->type) {
        fail(
            SqlExecutionErrorKind::Semantic,
            "comparison operands have incompatible types",
            span);
    }
}

void requireBoolean(const BoundExpression& expression) {
    if (expression.type != BoundType::Boolean && expression.type != BoundType::Null) {
        fail(
            SqlExecutionErrorKind::Semantic,
            "WHERE logical operand must be BOOLEAN",
            expression.span);
    }
}

std::unique_ptr<BoundExpression> bindExpression(
    const Expression& expression,
    const Schema& schema) {
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
        const auto index = resolveColumn(schema, identifier->name, expression.span);
        return std::make_unique<BoundExpression>(BoundExpression{
            expression.span,
            boundType(schema.column(index).type),
            BoundColumn{index},
        });
    }
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.node)) {
        return std::make_unique<BoundExpression>(BoundExpression{
            expression.span,
            literalType(literal->literal),
            BoundLiteral{literalScalar(literal->literal)},
        });
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
        auto operand = bindExpression(*unary->operand, schema);
        requireBoolean(*operand);
        return std::make_unique<BoundExpression>(BoundExpression{
            expression.span,
            BoundType::Boolean,
            BoundUnary{std::move(operand)},
        });
    }

    const auto& binary = std::get<BinaryExpression>(expression.node);
    auto left = bindExpression(*binary.left, schema);
    auto right = bindExpression(*binary.right, schema);
    if (binary.op == BinaryOperator::And || binary.op == BinaryOperator::Or) {
        requireBoolean(*left);
        requireBoolean(*right);
    } else {
        validateComparisonTypes(
            binary.op,
            left,
            right,
            *binary.left,
            *binary.right,
            schema,
            expression.span);
    }
    return std::make_unique<BoundExpression>(BoundExpression{
        expression.span,
        BoundType::Boolean,
        BoundBinary{binary.op, std::move(left), std::move(right)},
    });
}

int compareIntegerLiterals(const IntegerLiteral& left, const IntegerLiteral& right) {
    const auto normalized = [](const IntegerLiteral& value) {
        const auto first = value.magnitude.find_first_not_of('0');
        std::string_view magnitude = first == std::string::npos
            ? std::string_view{"0"}
            : std::string_view(value.magnitude).substr(first);
        const bool negative = value.negative && magnitude != "0";
        return std::pair{negative, magnitude};
    };
    const auto [leftNegative, leftMagnitude] = normalized(left);
    const auto [rightNegative, rightMagnitude] = normalized(right);
    if (leftNegative != rightNegative) {
        return leftNegative ? -1 : 1;
    }
    int comparison = 0;
    if (leftMagnitude.size() != rightMagnitude.size()) {
        comparison = leftMagnitude.size() < rightMagnitude.size() ? -1 : 1;
    } else if (leftMagnitude != rightMagnitude) {
        comparison = leftMagnitude < rightMagnitude ? -1 : 1;
    }
    return leftNegative ? -comparison : comparison;
}

int compareScalars(const BoundScalar& left, const BoundScalar& right) {
    if (const auto* leftValue = std::get_if<std::uint32_t>(&left)) {
        const auto rightValue = std::get<std::uint32_t>(right);
        return *leftValue < rightValue ? -1 : (*leftValue > rightValue ? 1 : 0);
    }
    if (const auto* leftValue = std::get_if<std::int64_t>(&left)) {
        const auto rightValue = std::get<std::int64_t>(right);
        return *leftValue < rightValue ? -1 : (*leftValue > rightValue ? 1 : 0);
    }
    if (const auto* leftValue = std::get_if<bool>(&left)) {
        const auto rightValue = std::get<bool>(right);
        return *leftValue == rightValue ? 0 : (*leftValue ? 1 : -1);
    }
    if (const auto* leftValue = std::get_if<std::string>(&left)) {
        const auto& rightValue = std::get<std::string>(right);
        return *leftValue < rightValue ? -1 : (*leftValue > rightValue ? 1 : 0);
    }
    return compareIntegerLiterals(
        std::get<IntegerLiteral>(left), std::get<IntegerLiteral>(right));
}

BoundScalar evaluateScalar(const BoundExpression& expression, const RowValues& row);

TruthValue evaluateTruth(const BoundExpression& expression, const RowValues& row) {
    if (const auto* unary = std::get_if<BoundUnary>(&expression.node)) {
        return truthNot(evaluateTruth(*unary->operand, row));
    }
    if (const auto* binary = std::get_if<BoundBinary>(&expression.node)) {
        if (binary->op == BinaryOperator::And) {
            const auto left = evaluateTruth(*binary->left, row);
            if (left == TruthValue::False) {
                return TruthValue::False;
            }
            return truthAnd(left, evaluateTruth(*binary->right, row));
        }
        if (binary->op == BinaryOperator::Or) {
            const auto left = evaluateTruth(*binary->left, row);
            if (left == TruthValue::True) {
                return TruthValue::True;
            }
            return truthOr(left, evaluateTruth(*binary->right, row));
        }
        const auto left = evaluateScalar(*binary->left, row);
        const auto right = evaluateScalar(*binary->right, row);
        if (std::holds_alternative<std::monostate>(left)
            || std::holds_alternative<std::monostate>(right)) {
            return TruthValue::Unknown;
        }
        const auto comparison = compareScalars(left, right);
        bool result = false;
        switch (binary->op) {
        case BinaryOperator::Equal: result = comparison == 0; break;
        case BinaryOperator::NotEqual: result = comparison != 0; break;
        case BinaryOperator::Less: result = comparison < 0; break;
        case BinaryOperator::LessEqual: result = comparison <= 0; break;
        case BinaryOperator::Greater: result = comparison > 0; break;
        case BinaryOperator::GreaterEqual: result = comparison >= 0; break;
        case BinaryOperator::And:
        case BinaryOperator::Or:
            throw std::logic_error("Logical operator reached scalar comparison.");
        }
        return result ? TruthValue::True : TruthValue::False;
    }
    const auto scalar = evaluateScalar(expression, row);
    if (std::holds_alternative<std::monostate>(scalar)) {
        return TruthValue::Unknown;
    }
    return std::get<bool>(scalar) ? TruthValue::True : TruthValue::False;
}

BoundScalar evaluateScalar(const BoundExpression& expression, const RowValues& row) {
    if (const auto* column = std::get_if<BoundColumn>(&expression.node)) {
        return valueScalar(row.at(column->index));
    }
    if (const auto* literal = std::get_if<BoundLiteral>(&expression.node)) {
        return literal->value;
    }
    const auto truth = evaluateTruth(expression, row);
    if (truth == TruthValue::Unknown) {
        return std::monostate{};
    }
    return truth == TruthValue::True;
}

std::optional<IndexKey> primaryKeyCandidate(
    const BoundExpression& expression,
    std::size_t primaryKeyColumn) {
    const auto* binary = std::get_if<BoundBinary>(&expression.node);
    if (binary == nullptr) {
        return std::nullopt;
    }
    if (binary->op == BinaryOperator::And) {
        const auto left = primaryKeyCandidate(*binary->left, primaryKeyColumn);
        return left.has_value()
            ? left
            : primaryKeyCandidate(*binary->right, primaryKeyColumn);
    }
    if (binary->op != BinaryOperator::Equal) {
        return std::nullopt;
    }
    const auto match = [primaryKeyColumn](
                           const BoundExpression& columnExpression,
                           const BoundExpression& literalExpression)
        -> std::optional<IndexKey> {
        const auto* column = std::get_if<BoundColumn>(&columnExpression.node);
        const auto* literal = std::get_if<BoundLiteral>(&literalExpression.node);
        if (column == nullptr || column->index != primaryKeyColumn || literal == nullptr) {
            return std::nullopt;
        }
        const auto* key = std::get_if<std::uint32_t>(&literal->value);
        return key == nullptr ? std::nullopt : std::optional<IndexKey>{*key};
    };
    auto candidate = match(*binary->left, *binary->right);
    return candidate.has_value() ? candidate : match(*binary->right, *binary->left);
}

std::vector<TableRow> matchingRows(
    Table& table,
    const BoundExpression* where,
    ExecutionStats& stats) {
    std::vector<TableRow> matches;
    const auto primaryKeyColumn = table.schema().primaryKeyColumn();
    const auto candidate = where != nullptr && primaryKeyColumn.has_value()
        ? primaryKeyCandidate(*where, *primaryKeyColumn)
        : std::nullopt;
    if (candidate.has_value()) {
        stats.accessPath = AccessPath::PrimaryKeyLookup;
        stats.indexLookups = 1;
        auto row = table.findByPrimaryKey(*candidate);
        if (row.has_value()) {
            ++stats.rowsExamined;
            if (where == nullptr || evaluateTruth(*where, row->values) == TruthValue::True) {
                matches.push_back(std::move(*row));
            }
        }
    } else {
        stats.accessPath = AccessPath::HeapScan;
        for (auto& row : table.scan()) {
            ++stats.rowsExamined;
            if (where == nullptr || evaluateTruth(*where, row.values) == TruthValue::True) {
                matches.push_back(std::move(row));
            }
        }
    }
    stats.rowsReturned = matches.size();
    return matches;
}

std::uint32_t parseVarcharSize(const SqlTypeSpecification& type) {
    if (!type.varcharSizeMagnitude.has_value()) {
        fail(
            SqlExecutionErrorKind::Semantic,
            "VARCHAR type is missing its length",
            type.span);
    }
    const ColumnDefinition target{"VARCHAR length", DataType::UINT32, false, false, 0};
    const SqlLiteral literal{
        type.span,
        IntegerLiteral{false, *type.varcharSizeMagnitude},
    };
    const auto value = std::get<std::uint32_t>(convertLiteral(literal, target));
    if (value == 0 || value > MAX_VARCHAR_BYTES) {
        fail(
            SqlExecutionErrorKind::Semantic,
            "VARCHAR length must be between 1 and " + std::to_string(MAX_VARCHAR_BYTES),
            type.span);
    }
    return value;
}

CommandResult executeCreate(Catalog& catalog, const CreateTableStatement& statement) {
    std::unordered_set<std::string> normalizedNames;
    std::vector<ColumnDefinition> columns;
    columns.reserve(statement.columns.size());
    std::size_t primaryKeyCount = 0;
    for (const auto& specification : statement.columns) {
        std::string name;
        try {
            name = normalizeIdentifier(specification.name);
        } catch (const std::invalid_argument& error) {
            fail(SqlExecutionErrorKind::Semantic, error.what(), specification.span);
        }
        if (!normalizedNames.insert(name).second) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "duplicate column name '" + specification.name + "'",
                specification.span);
        }
        DataType type = DataType::UINT32;
        std::uint32_t varcharMaximum = 0;
        switch (specification.type.kind) {
        case SqlTypeKind::Uint32: type = DataType::UINT32; break;
        case SqlTypeKind::Int64: type = DataType::INT64; break;
        case SqlTypeKind::Boolean: type = DataType::BOOLEAN; break;
        case SqlTypeKind::Varchar:
            type = DataType::VARCHAR;
            varcharMaximum = parseVarcharSize(specification.type);
            break;
        }
        if (specification.primaryKey) {
            ++primaryKeyCount;
            if (primaryKeyCount > 1) {
                fail(
                    SqlExecutionErrorKind::Semantic,
                    "CREATE TABLE declares more than one primary key",
                    specification.span);
            }
            if (specification.nullConstraint == NullConstraint::Null) {
                fail(
                    SqlExecutionErrorKind::Semantic,
                    "primary-key column cannot be nullable",
                    specification.span);
            }
            if (type != DataType::UINT32) {
                fail(
                    SqlExecutionErrorKind::Semantic,
                    "MiniDB++ primary keys must use UINT32",
                    specification.type.span);
            }
        }
        const bool nullable = specification.nullConstraint == NullConstraint::Null
            || (specification.nullConstraint == NullConstraint::Unspecified
                && !specification.primaryKey);
        columns.push_back(ColumnDefinition{
            std::move(name), type, nullable, specification.primaryKey, varcharMaximum,
        });
    }

    try {
        const auto normalizedTable = normalizeIdentifier(statement.tableName);
        if (catalog.findTable(normalizedTable).has_value()) {
            fail(
                SqlExecutionErrorKind::Constraint,
                "table '" + statement.tableName + "' already exists",
                statement.tableNameSpan);
        }
        const auto schema = Schema::create(std::move(columns));
        static_cast<void>(catalog.createTable(normalizedTable, schema));
    } catch (const SqlExecutionError&) {
        throw;
    } catch (const std::invalid_argument& error) {
        fail(SqlExecutionErrorKind::Semantic, error.what(), statement.tableNameSpan);
    }
    return CommandResult{CommandKind::CreateTable, 0, std::nullopt, {}};
}

CommandResult executeInsert(Catalog& catalog, const InsertStatement& statement) {
    auto table = openTable(catalog, statement.tableName, statement.tableNameSpan);
    const auto& schema = table.schema();
    RowValues row(schema.columnCount(), std::monostate{});
    std::vector<SourceSpan> valueSpans(schema.columnCount(), statement.tableNameSpan);

    if (!statement.columns.has_value()) {
        if (statement.values.size() != schema.columnCount()) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "INSERT value count does not match table column count",
                statement.values.empty() ? statement.tableNameSpan : statement.values.back().span);
        }
        for (std::size_t index = 0; index < schema.columnCount(); ++index) {
            row[index] = convertLiteral(statement.values[index], schema.column(index));
            valueSpans[index] = statement.values[index].span;
        }
    } else {
        if (statement.columns->size() != statement.values.size()) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "INSERT column count does not match value count",
                statement.tableNameSpan);
        }
        std::unordered_set<std::size_t> assigned;
        for (std::size_t index = 0; index < statement.columns->size(); ++index) {
            const auto span = (*statement.columnSpans)[index];
            const auto columnIndex = resolveColumn(schema, (*statement.columns)[index], span);
            if (!assigned.insert(columnIndex).second) {
                fail(
                    SqlExecutionErrorKind::Semantic,
                    "duplicate INSERT column '" + (*statement.columns)[index] + "'",
                    span);
            }
            row[columnIndex] = convertLiteral(statement.values[index], schema.column(columnIndex));
            valueSpans[columnIndex] = statement.values[index].span;
        }
        for (std::size_t index = 0; index < schema.columnCount(); ++index) {
            if (!assigned.contains(index) && !schema.column(index).nullable) {
                fail(
                    SqlExecutionErrorKind::Constraint,
                    "required column '" + schema.column(index).name + "' was omitted",
                    statement.tableNameSpan);
            }
        }
    }

    try {
        const auto recordId = table.insert(row);
        return CommandResult{CommandKind::Insert, 1, recordId, {}};
    } catch (const std::invalid_argument& error) {
        const auto primaryKey = schema.primaryKeyColumn();
        const auto span = primaryKey.has_value() ? valueSpans[*primaryKey] : statement.tableNameSpan;
        fail(SqlExecutionErrorKind::Constraint, error.what(), span);
    }
}

std::vector<std::size_t> bindProjection(
    const SelectStatement& statement,
    const Schema& schema,
    std::vector<std::string>& names) {
    std::vector<std::size_t> projection;
    if (statement.selectAll) {
        projection.reserve(schema.columnCount());
        names.reserve(schema.columnCount());
        for (std::size_t index = 0; index < schema.columnCount(); ++index) {
            projection.push_back(index);
            names.push_back(schema.column(index).name);
        }
        return projection;
    }
    projection.reserve(statement.columns.size());
    names.reserve(statement.columns.size());
    for (std::size_t index = 0; index < statement.columns.size(); ++index) {
        const auto columnIndex = resolveColumn(
            schema, statement.columns[index], statement.columnSpans[index]);
        projection.push_back(columnIndex);
        names.push_back(schema.column(columnIndex).name);
    }
    return projection;
}

SelectResult executeSelect(Catalog& catalog, const SelectStatement& statement) {
    auto table = openTable(catalog, statement.tableName, statement.tableNameSpan);
    std::vector<std::string> columnNames;
    const auto projection = bindProjection(statement, table.schema(), columnNames);
    const auto where = statement.where == nullptr
        ? std::unique_ptr<BoundExpression>{}
        : bindExpression(*statement.where, table.schema());
    if (where != nullptr) {
        requireBoolean(*where);
    }
    ExecutionStats stats;
    auto matches = matchingRows(table, where.get(), stats);
    SelectResult result;
    result.columns = std::move(columnNames);
    result.stats = stats;
    result.rows.reserve(matches.size());
    result.recordIds.reserve(matches.size());
    for (auto& match : matches) {
        RowValues projected;
        projected.reserve(projection.size());
        for (const auto index : projection) {
            projected.push_back(match.values[index]);
        }
        result.recordIds.push_back(match.recordId);
        result.rows.push_back(std::move(projected));
    }
    return result;
}

std::vector<BoundAssignment> bindAssignments(
    const UpdateStatement& statement,
    const Schema& schema) {
    std::vector<BoundAssignment> assignments;
    assignments.reserve(statement.assignments.size());
    std::unordered_set<std::size_t> assigned;
    for (const auto& assignment : statement.assignments) {
        const auto columnIndex = resolveColumn(
            schema, assignment.columnName, assignment.columnNameSpan);
        if (!assigned.insert(columnIndex).second) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "duplicate UPDATE assignment for column '" + assignment.columnName + "'",
                assignment.columnNameSpan);
        }
        assignments.push_back(BoundAssignment{
            columnIndex,
            convertLiteral(assignment.value, schema.column(columnIndex)),
            assignment.span,
        });
    }
    return assignments;
}

void prevalidateUpdatedPrimaryKeys(
    Table& table,
    const std::vector<TableRow>& targets,
    const std::vector<RowValues>& replacements,
    SourceSpan span) {
    const auto primaryKeyColumn = table.schema().primaryKeyColumn();
    if (!primaryKeyColumn.has_value()) {
        return;
    }
    std::unordered_set<IndexKey> targetOldKeys;
    std::unordered_set<IndexKey> replacementKeys;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        targetOldKeys.insert(std::get<std::uint32_t>(targets[index].values[*primaryKeyColumn]));
        const auto replacement = std::get<std::uint32_t>(replacements[index][*primaryKeyColumn]);
        if (!replacementKeys.insert(replacement).second) {
            fail(
                SqlExecutionErrorKind::Constraint,
                "UPDATE would produce duplicate primary key " + std::to_string(replacement),
                span);
        }
    }
    for (const auto replacement : replacementKeys) {
        const auto existing = table.findByPrimaryKey(replacement);
        if (existing.has_value()) {
            const auto oldKey = std::get<std::uint32_t>(
                existing->values[*primaryKeyColumn]);
            if (!targetOldKeys.contains(oldKey)) {
                fail(
                    SqlExecutionErrorKind::Constraint,
                    "UPDATE primary key " + std::to_string(replacement)
                        + " already exists",
                    span);
            }
        }
    }
}

CommandResult executeUpdate(Catalog& catalog, const UpdateStatement& statement) {
    auto table = openTable(catalog, statement.tableName, statement.tableNameSpan);
    const auto assignments = bindAssignments(statement, table.schema());
    const auto where = statement.where == nullptr
        ? std::unique_ptr<BoundExpression>{}
        : bindExpression(*statement.where, table.schema());
    if (where != nullptr) {
        requireBoolean(*where);
    }

    ExecutionStats stats;
    auto targets = matchingRows(table, where.get(), stats);
    std::vector<RowValues> replacements;
    replacements.reserve(targets.size());
    for (const auto& target : targets) {
        auto values = target.values;
        for (const auto& assignment : assignments) {
            values[assignment.columnIndex] = assignment.value;
        }
        try {
            table.schema().validateValues(values);
        } catch (const std::invalid_argument& error) {
            fail(SqlExecutionErrorKind::Constraint, error.what(), statement.tableNameSpan);
        }
        replacements.push_back(std::move(values));
    }
    auto primaryKeySpan = statement.tableNameSpan;
    const auto primaryKeyColumn = table.schema().primaryKeyColumn();
    if (primaryKeyColumn.has_value()) {
        const auto assignment = std::find_if(
            assignments.begin(), assignments.end(),
            [&](const BoundAssignment& item) {
                return item.columnIndex == *primaryKeyColumn;
            });
        if (assignment != assignments.end()) {
            primaryKeySpan = assignment->span;
        }
    }
    prevalidateUpdatedPrimaryKeys(table, targets, replacements, primaryKeySpan);

    for (std::size_t index = 0; index < targets.size(); ++index) {
        try {
            static_cast<void>(table.update(targets[index].recordId, replacements[index]));
        } catch (const std::invalid_argument& error) {
            fail(SqlExecutionErrorKind::Constraint, error.what(), statement.tableNameSpan);
        }
    }
    return CommandResult{CommandKind::Update, targets.size(), std::nullopt, stats};
}

CommandResult executeDelete(Catalog& catalog, const DeleteStatement& statement) {
    auto table = openTable(catalog, statement.tableName, statement.tableNameSpan);
    const auto where = statement.where == nullptr
        ? std::unique_ptr<BoundExpression>{}
        : bindExpression(*statement.where, table.schema());
    if (where != nullptr) {
        requireBoolean(*where);
    }
    ExecutionStats stats;
    auto targets = matchingRows(table, where.get(), stats);
    for (const auto& target : targets) {
        table.erase(target.recordId);
    }
    return CommandResult{CommandKind::Delete, targets.size(), std::nullopt, stats};
}

} // namespace

QueryResult SqlExecutor::execute(const Statement& statement) {
    try {
        return std::visit(
            [this](const auto& node) -> QueryResult {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, CreateTableStatement>) {
                    return executeCreate(catalog_, node);
                } else if constexpr (std::is_same_v<Node, InsertStatement>) {
                    return executeInsert(catalog_, node);
                } else if constexpr (std::is_same_v<Node, SelectStatement>) {
                    return executeSelect(catalog_, node);
                } else if constexpr (std::is_same_v<Node, UpdateStatement>) {
                    return executeUpdate(catalog_, node);
                } else {
                    return executeDelete(catalog_, node);
                }
            },
            statement.node);
    } catch (const SqlExecutionError&) {
        throw;
    } catch (const std::exception& error) {
        throw SqlExecutionError(
            SqlExecutionErrorKind::Execution,
            std::string("SQL execution failed: ") + error.what(),
            statement.span);
    }
}

QueryResult SqlEngine::execute(std::string_view source) {
    auto statement = Parser::parse(source);
    return executor_.execute(statement);
}

} // namespace minidb::sql
