#include "minidb/sql_parser.hpp"

#include "minidb/sql_lexer.hpp"

#include <stdexcept>
#include <utility>

namespace minidb::sql {
namespace {

bool isComparison(TokenKind kind) noexcept {
    return kind == TokenKind::Equal || kind == TokenKind::BangEqual
        || kind == TokenKind::LessGreater || kind == TokenKind::Less
        || kind == TokenKind::LessEqual || kind == TokenKind::Greater
        || kind == TokenKind::GreaterEqual;
}

BinaryOperator comparisonOperator(TokenKind kind) {
    switch (kind) {
    case TokenKind::Equal: return BinaryOperator::Equal;
    case TokenKind::BangEqual:
    case TokenKind::LessGreater: return BinaryOperator::NotEqual;
    case TokenKind::Less: return BinaryOperator::Less;
    case TokenKind::LessEqual: return BinaryOperator::LessEqual;
    case TokenKind::Greater: return BinaryOperator::Greater;
    case TokenKind::GreaterEqual: return BinaryOperator::GreaterEqual;
    default: throw std::logic_error("Token is not a comparison operator.");
    }
}

std::unique_ptr<Expression> makeBinary(
    BinaryOperator op,
    std::unique_ptr<Expression> left,
    std::unique_ptr<Expression> right) {
    const SourceSpan span{left->span.begin, right->span.end};
    return std::make_unique<Expression>(Expression{
        span,
        BinaryExpression{op, std::move(left), std::move(right)},
    });
}

} // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    if (tokens_.empty() || tokens_.back().kind != TokenKind::EndOfFile) {
        const SourceLocation location{};
        throw SqlError(
            SqlErrorKind::Parser,
            "token stream must end with an explicit EOF token",
            SourceSpan{location, location});
    }
}

Statement Parser::parse(std::string_view source) {
    return Parser(Lexer(source).tokenize()).parseStatement();
}

const Token& Parser::peek() const noexcept {
    return tokens_[current_ < tokens_.size() ? current_ : tokens_.size() - 1];
}

const Token& Parser::previous() const noexcept {
    return tokens_[current_ == 0 ? 0 : current_ - 1];
}

bool Parser::check(TokenKind kind) const noexcept {
    return peek().kind == kind;
}

const Token& Parser::advance() noexcept {
    if (!check(TokenKind::EndOfFile)) {
        ++current_;
    }
    return previous();
}

bool Parser::match(TokenKind kind) noexcept {
    if (!check(kind)) {
        return false;
    }
    static_cast<void>(advance());
    return true;
}

const Token& Parser::consume(TokenKind kind, std::string message) {
    if (check(kind)) {
        return advance();
    }
    failAt(peek(), std::move(message));
}

[[noreturn]] void Parser::failAt(const Token& token, std::string message) const {
    throw SqlError(SqlErrorKind::Parser, std::move(message), token.span);
}

Statement Parser::parseStatement() {
    const auto begin = peek().span.begin;
    StatementNode node;
    if (match(TokenKind::Create)) {
        node = parseCreateTable();
    } else if (match(TokenKind::Insert)) {
        node = parseInsert();
    } else if (match(TokenKind::Select)) {
        node = parseSelect();
    } else if (match(TokenKind::Update)) {
        node = parseUpdate();
    } else if (match(TokenKind::Delete)) {
        node = parseDelete();
    } else {
        failAt(peek(), "expected CREATE, INSERT, SELECT, UPDATE, or DELETE statement");
    }

    auto end = previous().span.end;
    if (match(TokenKind::Semicolon)) {
        end = previous().span.end;
    }
    consume(TokenKind::EndOfFile, "expected end of input after one SQL statement");
    return Statement{SourceSpan{begin, end}, std::move(node)};
}

CreateTableStatement Parser::parseCreateTable() {
    consume(TokenKind::Table, "expected TABLE after CREATE");
    const auto& table = consume(TokenKind::Identifier, "expected table name after CREATE TABLE");
    consume(TokenKind::LeftParen, "expected '(' before column definitions");
    if (check(TokenKind::RightParen)) {
        failAt(peek(), "CREATE TABLE requires at least one column definition");
    }
    std::vector<ColumnSpecification> columns;
    columns.push_back(parseColumnSpecification());
    while (match(TokenKind::Comma)) {
        if (check(TokenKind::RightParen)) {
            failAt(peek(), "trailing comma is not allowed after a column definition");
        }
        columns.push_back(parseColumnSpecification());
    }
    consume(TokenKind::RightParen, "expected ')' after column definitions");
    return CreateTableStatement{table.value, std::move(columns)};
}

ColumnSpecification Parser::parseColumnSpecification() {
    const auto& name = consume(TokenKind::Identifier, "expected column name");
    auto type = parseTypeSpecification();
    NullConstraint nullConstraint = NullConstraint::Unspecified;
    bool primaryKey = false;
    auto end = type.span.end;
    while (check(TokenKind::Primary) || check(TokenKind::Not) || check(TokenKind::Null)) {
        if (match(TokenKind::Primary)) {
            if (primaryKey) {
                failAt(previous(), "duplicate PRIMARY KEY constraint");
            }
            consume(TokenKind::Key, "expected KEY after PRIMARY");
            primaryKey = true;
            end = previous().span.end;
        } else if (match(TokenKind::Not)) {
            const auto notToken = previous();
            consume(TokenKind::Null, "expected NULL after NOT");
            if (nullConstraint != NullConstraint::Unspecified) {
                failAt(notToken, "duplicate or contradictory NULL constraint");
            }
            nullConstraint = NullConstraint::NotNull;
            end = previous().span.end;
        } else {
            const auto nullToken = advance();
            if (nullConstraint != NullConstraint::Unspecified) {
                failAt(nullToken, "duplicate or contradictory NULL constraint");
            }
            nullConstraint = NullConstraint::Null;
            end = nullToken.span.end;
        }
    }
    return ColumnSpecification{
        name.value,
        std::move(type),
        nullConstraint,
        primaryKey,
        SourceSpan{name.span.begin, end},
    };
}

SqlTypeSpecification Parser::parseTypeSpecification() {
    const auto begin = peek().span.begin;
    if (match(TokenKind::Uint32)) {
        return SqlTypeSpecification{SqlTypeKind::Uint32, std::nullopt,
                                    SourceSpan{begin, previous().span.end}};
    }
    if (match(TokenKind::Int64)) {
        return SqlTypeSpecification{SqlTypeKind::Int64, std::nullopt,
                                    SourceSpan{begin, previous().span.end}};
    }
    if (match(TokenKind::Boolean)) {
        return SqlTypeSpecification{SqlTypeKind::Boolean, std::nullopt,
                                    SourceSpan{begin, previous().span.end}};
    }
    if (match(TokenKind::Varchar)) {
        consume(TokenKind::LeftParen, "expected '(' after VARCHAR");
        const auto& size = consume(TokenKind::Integer, "expected unsigned VARCHAR size");
        consume(TokenKind::RightParen, "expected ')' after VARCHAR size");
        return SqlTypeSpecification{
            SqlTypeKind::Varchar,
            size.value,
            SourceSpan{begin, previous().span.end},
        };
    }
    failAt(peek(), "expected UINT32, INT64, BOOLEAN, or VARCHAR type");
}

std::vector<std::string> Parser::parseIdentifierList(std::string context) {
    if (check(TokenKind::RightParen)) {
        failAt(peek(), context + " cannot be empty");
    }
    std::vector<std::string> identifiers;
    identifiers.push_back(consume(TokenKind::Identifier, "expected identifier in " + context).value);
    while (match(TokenKind::Comma)) {
        if (check(TokenKind::RightParen)) {
            failAt(peek(), "trailing comma is not allowed in " + context);
        }
        identifiers.push_back(
            consume(TokenKind::Identifier, "expected identifier after comma in " + context).value);
    }
    return identifiers;
}

std::vector<SqlLiteral> Parser::parseLiteralList() {
    if (check(TokenKind::RightParen)) {
        failAt(peek(), "VALUES list cannot be empty");
    }
    std::vector<SqlLiteral> values;
    values.push_back(parseLiteral());
    while (match(TokenKind::Comma)) {
        if (check(TokenKind::RightParen)) {
            failAt(peek(), "trailing comma is not allowed in VALUES list");
        }
        values.push_back(parseLiteral());
    }
    return values;
}

InsertStatement Parser::parseInsert() {
    consume(TokenKind::Into, "expected INTO after INSERT");
    const auto& table = consume(TokenKind::Identifier, "expected table name after INSERT INTO");
    std::optional<std::vector<std::string>> columns;
    if (match(TokenKind::LeftParen)) {
        columns = parseIdentifierList("INSERT column list");
        consume(TokenKind::RightParen, "expected ')' after INSERT column list");
    }
    consume(TokenKind::Values, "expected VALUES in INSERT statement");
    consume(TokenKind::LeftParen, "expected '(' before VALUES list");
    auto values = parseLiteralList();
    consume(TokenKind::RightParen, "expected ')' after VALUES list");
    return InsertStatement{table.value, std::move(columns), std::move(values)};
}

SelectStatement Parser::parseSelect() {
    bool selectAll = false;
    std::vector<std::string> columns;
    if (match(TokenKind::Star)) {
        selectAll = true;
    } else {
        if (!check(TokenKind::Identifier)) {
            failAt(peek(), "expected '*' or column identifier after SELECT");
        }
        columns.push_back(advance().value);
        while (match(TokenKind::Comma)) {
            if (check(TokenKind::Star)) {
                failAt(peek(), "cannot mix '*' with named SELECT projections");
            }
            columns.push_back(
                consume(TokenKind::Identifier, "expected column identifier after comma").value);
        }
    }
    consume(TokenKind::From, "expected FROM after SELECT projection");
    const auto& table = consume(TokenKind::Identifier, "expected table name after FROM");
    std::unique_ptr<Expression> where;
    if (match(TokenKind::Where)) {
        where = parseExpression();
    }
    return SelectStatement{selectAll, std::move(columns), table.value, std::move(where)};
}

UpdateStatement Parser::parseUpdate() {
    const auto& table = consume(TokenKind::Identifier, "expected table name after UPDATE");
    consume(TokenKind::Set, "expected SET after UPDATE table name");
    if (!check(TokenKind::Identifier)) {
        failAt(peek(), "UPDATE SET requires at least one assignment");
    }
    std::vector<Assignment> assignments;
    while (true) {
        const auto& column = consume(TokenKind::Identifier, "expected assignment column name");
        consume(TokenKind::Equal, "expected '=' after assignment column");
        auto value = parseLiteral();
        assignments.push_back(Assignment{
            column.value,
            std::move(value),
            SourceSpan{column.span.begin, previous().span.end},
        });
        if (!match(TokenKind::Comma)) {
            break;
        }
        if (!check(TokenKind::Identifier)) {
            failAt(peek(), "expected assignment after comma");
        }
    }
    std::unique_ptr<Expression> where;
    if (match(TokenKind::Where)) {
        where = parseExpression();
    }
    return UpdateStatement{table.value, std::move(assignments), std::move(where)};
}

DeleteStatement Parser::parseDelete() {
    consume(TokenKind::From, "expected FROM after DELETE");
    const auto& table = consume(TokenKind::Identifier, "expected table name after DELETE FROM");
    std::unique_ptr<Expression> where;
    if (match(TokenKind::Where)) {
        where = parseExpression();
    }
    return DeleteStatement{table.value, std::move(where)};
}

SqlLiteral Parser::parseLiteral() {
    const auto begin = peek().span.begin;
    bool negative = false;
    if (match(TokenKind::Minus)) {
        negative = true;
        if (!check(TokenKind::Integer)) {
            failAt(peek(), "expected integer magnitude after '-'");
        }
    }
    if (match(TokenKind::Integer)) {
        return SqlLiteral{
            SourceSpan{begin, previous().span.end},
            IntegerLiteral{negative, previous().value},
        };
    }
    if (negative) {
        failAt(peek(), "expected integer magnitude after '-'");
    }
    if (match(TokenKind::String)) {
        return SqlLiteral{previous().span, StringLiteral{previous().value}};
    }
    if (match(TokenKind::True)) {
        return SqlLiteral{previous().span, BooleanLiteral{true}};
    }
    if (match(TokenKind::False)) {
        return SqlLiteral{previous().span, BooleanLiteral{false}};
    }
    if (match(TokenKind::Null)) {
        return SqlLiteral{previous().span, NullLiteral{}};
    }
    failAt(peek(), "expected NULL, TRUE, FALSE, integer, or string literal");
}

std::unique_ptr<Expression> Parser::parseExpression(std::size_t depth) {
    return parseOr(depth);
}

std::unique_ptr<Expression> Parser::parseOr(std::size_t depth) {
    auto expression = parseAnd(depth);
    while (match(TokenKind::Or)) {
        expression = makeBinary(
            BinaryOperator::Or, std::move(expression), parseAnd(depth));
    }
    return expression;
}

std::unique_ptr<Expression> Parser::parseAnd(std::size_t depth) {
    auto expression = parseNot(depth);
    while (match(TokenKind::And)) {
        expression = makeBinary(
            BinaryOperator::And, std::move(expression), parseNot(depth));
    }
    return expression;
}

void Parser::requireNestingAvailable(const Token& token, std::size_t depth) const {
    if (depth >= MAX_EXPRESSION_NESTING) {
        failAt(token, "expression nesting exceeds the limit of 128");
    }
}

std::unique_ptr<Expression> Parser::parseNot(std::size_t depth) {
    if (match(TokenKind::Not)) {
        const auto notToken = previous();
        requireNestingAvailable(notToken, depth);
        auto operand = parseNot(depth + 1);
        return std::make_unique<Expression>(Expression{
            SourceSpan{notToken.span.begin, operand->span.end},
            UnaryExpression{UnaryOperator::Not, std::move(operand)},
        });
    }
    return parseComparison(depth);
}

std::unique_ptr<Expression> Parser::parseComparison(std::size_t depth) {
    auto left = parsePrimary(depth);
    if (!isComparison(peek().kind)) {
        return left;
    }
    const auto op = comparisonOperator(advance().kind);
    auto expression = makeBinary(op, std::move(left), parsePrimary(depth));
    if (isComparison(peek().kind)) {
        failAt(peek(), "chained comparisons are not supported; use AND explicitly");
    }
    return expression;
}

std::unique_ptr<Expression> Parser::parsePrimary(std::size_t depth) {
    if (match(TokenKind::Identifier)) {
        return std::make_unique<Expression>(Expression{
            previous().span,
            IdentifierExpression{previous().value},
        });
    }
    if (check(TokenKind::Integer) || check(TokenKind::Minus) || check(TokenKind::String)
        || check(TokenKind::True) || check(TokenKind::False) || check(TokenKind::Null)) {
        auto literal = parseLiteral();
        const auto span = literal.span;
        return std::make_unique<Expression>(Expression{
            span,
            LiteralExpression{std::move(literal)},
        });
    }
    if (match(TokenKind::LeftParen)) {
        const auto leftParen = previous();
        requireNestingAvailable(leftParen, depth);
        auto expression = parseExpression(depth + 1);
        const auto& rightParen = consume(
            TokenKind::RightParen, "expected ')' after parenthesized expression");
        expression->span = SourceSpan{leftParen.span.begin, rightParen.span.end};
        return expression;
    }
    failAt(peek(), "expected identifier, literal, or parenthesized expression");
}

} // namespace minidb::sql
