#include "minidb/sql_ast.hpp"

#include <type_traits>

namespace minidb::sql {
namespace {

template <class... Types>
struct Overloaded : Types... {
    using Types::operator()...;
};

std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result += '"';
    return result;
}

std::string binaryName(BinaryOperator op) {
    switch (op) {
    case BinaryOperator::Equal: return "Eq";
    case BinaryOperator::NotEqual: return "Ne";
    case BinaryOperator::Less: return "Lt";
    case BinaryOperator::LessEqual: return "Le";
    case BinaryOperator::Greater: return "Gt";
    case BinaryOperator::GreaterEqual: return "Ge";
    case BinaryOperator::And: return "And";
    case BinaryOperator::Or: return "Or";
    }
    return "UnknownBinary";
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string result = "[";
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += names[index];
    }
    result += ']';
    return result;
}

} // namespace

std::string toDebugString(const SqlLiteral& literal) {
    return std::visit(Overloaded{
        [](const NullLiteral&) { return std::string("Null"); },
        [](const BooleanLiteral& value) {
            return std::string(value.value ? "Bool(true)" : "Bool(false)");
        },
        [](const IntegerLiteral& value) {
            return "Int(" + std::string(value.negative ? "-" : "") + value.magnitude + ')';
        },
        [](const StringLiteral& value) { return "String(" + quote(value.value) + ')'; },
    }, literal.value);
}

std::string toDebugString(const Expression& expression) {
    return std::visit(Overloaded{
        [](const IdentifierExpression& identifier) {
            return "Id(" + identifier.name + ')';
        },
        [](const LiteralExpression& literal) {
            return toDebugString(literal.literal);
        },
        [](const UnaryExpression& unary) {
            return "Not(" + toDebugString(*unary.operand) + ')';
        },
        [](const BinaryExpression& binary) {
            return binaryName(binary.op) + '(' + toDebugString(*binary.left)
                + ',' + toDebugString(*binary.right) + ')';
        },
    }, expression.node);
}

std::string toDebugString(const Statement& statement) {
    return std::visit(Overloaded{
        [](const CreateTableStatement& create) {
            std::string result = "Create(table=" + create.tableName + ", columns=[";
            for (std::size_t index = 0; index < create.columns.size(); ++index) {
                if (index != 0) {
                    result += ',';
                }
                const auto& column = create.columns[index];
                result += column.name + ':';
                switch (column.type.kind) {
                case SqlTypeKind::Uint32: result += "UINT32"; break;
                case SqlTypeKind::Int64: result += "INT64"; break;
                case SqlTypeKind::Boolean: result += "BOOLEAN"; break;
                case SqlTypeKind::Varchar:
                    result += "VARCHAR(" + *column.type.varcharSizeMagnitude + ')';
                    break;
                }
                if (column.primaryKey) {
                    result += ":PK";
                }
                if (column.nullConstraint == NullConstraint::Null) {
                    result += ":NULL";
                } else if (column.nullConstraint == NullConstraint::NotNull) {
                    result += ":NOT_NULL";
                }
            }
            return result + "])";
        },
        [](const InsertStatement& insert) {
            std::string result = "Insert(table=" + insert.tableName + ", columns=";
            result += insert.columns ? joinNames(*insert.columns) : "<implicit>";
            result += ", values=[";
            for (std::size_t index = 0; index < insert.values.size(); ++index) {
                if (index != 0) {
                    result += ',';
                }
                result += toDebugString(insert.values[index]);
            }
            return result + "])";
        },
        [](const SelectStatement& select) {
            const auto projection = select.selectAll ? std::string("*") : joinNames(select.columns);
            return "Select(columns=" + projection + ", table=" + select.tableName
                + ", where=" + (select.where ? toDebugString(*select.where) : "<none>") + ')';
        },
        [](const UpdateStatement& update) {
            std::string result = "Update(table=" + update.tableName + ", set=[";
            for (std::size_t index = 0; index < update.assignments.size(); ++index) {
                if (index != 0) {
                    result += ',';
                }
                result += update.assignments[index].columnName + '='
                    + toDebugString(update.assignments[index].value);
            }
            return result + "], where="
                + (update.where ? toDebugString(*update.where) : "<none>") + ')';
        },
        [](const DeleteStatement& deletion) {
            return "Delete(table=" + deletion.tableName + ", where="
                + (deletion.where ? toDebugString(*deletion.where) : "<none>") + ')';
        },
    }, statement.node);
}

} // namespace minidb::sql
