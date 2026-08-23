#include "minidb/sql_lexer.hpp"

#include <array>
#include <string>
#include <utility>

namespace minidb::sql {
namespace {

struct Keyword {
    std::string_view spelling;
    TokenKind kind;
};

constexpr std::array KEYWORDS{
    Keyword{"CREATE", TokenKind::Create},
    Keyword{"TABLE", TokenKind::Table},
    Keyword{"INSERT", TokenKind::Insert},
    Keyword{"INTO", TokenKind::Into},
    Keyword{"VALUES", TokenKind::Values},
    Keyword{"SELECT", TokenKind::Select},
    Keyword{"FROM", TokenKind::From},
    Keyword{"WHERE", TokenKind::Where},
    Keyword{"UPDATE", TokenKind::Update},
    Keyword{"SET", TokenKind::Set},
    Keyword{"DELETE", TokenKind::Delete},
    Keyword{"PRIMARY", TokenKind::Primary},
    Keyword{"KEY", TokenKind::Key},
    Keyword{"NOT", TokenKind::Not},
    Keyword{"NULL", TokenKind::Null},
    Keyword{"UINT32", TokenKind::Uint32},
    Keyword{"INT64", TokenKind::Int64},
    Keyword{"BOOLEAN", TokenKind::Boolean},
    Keyword{"VARCHAR", TokenKind::Varchar},
    Keyword{"TRUE", TokenKind::True},
    Keyword{"FALSE", TokenKind::False},
    Keyword{"AND", TokenKind::And},
    Keyword{"OR", TokenKind::Or},
};

bool isIdentifierStart(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

bool isIdentifierContinue(char value) noexcept {
    return isIdentifierStart(value) || (value >= '0' && value <= '9');
}

bool isDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

char asciiUpper(char value) noexcept {
    return (value >= 'a' && value <= 'z') ? static_cast<char>(value - 'a' + 'A') : value;
}

TokenKind keywordKind(std::string_view lexeme) noexcept {
    for (const auto& keyword : KEYWORDS) {
        if (keyword.spelling.size() != lexeme.size()) {
            continue;
        }
        bool equal = true;
        for (std::size_t index = 0; index < lexeme.size(); ++index) {
            if (asciiUpper(lexeme[index]) != keyword.spelling[index]) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return keyword.kind;
        }
    }
    return TokenKind::Identifier;
}

} // namespace

std::string_view tokenKindName(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::EndOfFile: return "end of input";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Integer: return "integer";
    case TokenKind::String: return "string";
    case TokenKind::Create: return "CREATE";
    case TokenKind::Table: return "TABLE";
    case TokenKind::Insert: return "INSERT";
    case TokenKind::Into: return "INTO";
    case TokenKind::Values: return "VALUES";
    case TokenKind::Select: return "SELECT";
    case TokenKind::From: return "FROM";
    case TokenKind::Where: return "WHERE";
    case TokenKind::Update: return "UPDATE";
    case TokenKind::Set: return "SET";
    case TokenKind::Delete: return "DELETE";
    case TokenKind::Primary: return "PRIMARY";
    case TokenKind::Key: return "KEY";
    case TokenKind::Not: return "NOT";
    case TokenKind::Null: return "NULL";
    case TokenKind::Uint32: return "UINT32";
    case TokenKind::Int64: return "INT64";
    case TokenKind::Boolean: return "BOOLEAN";
    case TokenKind::Varchar: return "VARCHAR";
    case TokenKind::True: return "TRUE";
    case TokenKind::False: return "FALSE";
    case TokenKind::And: return "AND";
    case TokenKind::Or: return "OR";
    case TokenKind::LeftParen: return "(";
    case TokenKind::RightParen: return ")";
    case TokenKind::Comma: return ",";
    case TokenKind::Semicolon: return ";";
    case TokenKind::Star: return "*";
    case TokenKind::Equal: return "=";
    case TokenKind::BangEqual: return "!=";
    case TokenKind::LessGreater: return "<>";
    case TokenKind::Less: return "<";
    case TokenKind::LessEqual: return "<=";
    case TokenKind::Greater: return ">";
    case TokenKind::GreaterEqual: return ">=";
    case TokenKind::Minus: return "-";
    }
    return "unknown token";
}

char Lexer::peek(std::size_t lookahead) const noexcept {
    return current_ + lookahead < source_.size() ? source_[current_ + lookahead] : '\0';
}

SourceLocation Lexer::location() const noexcept {
    return SourceLocation{current_, line_, column_};
}

char Lexer::advance() noexcept {
    const char value = source_[current_++];
    if (value == '\r') {
        ++line_;
        column_ = 1;
        previousWasCarriageReturn_ = true;
    } else if (value == '\n') {
        if (!previousWasCarriageReturn_) {
            ++line_;
        }
        column_ = 1;
        previousWasCarriageReturn_ = false;
    } else {
        ++column_;
        previousWasCarriageReturn_ = false;
    }
    return value;
}

bool Lexer::match(char expected) noexcept {
    if (atEnd() || peek() != expected) {
        return false;
    }
    static_cast<void>(advance());
    return true;
}

Token Lexer::makeToken(TokenKind kind, SourceLocation begin) const {
    const auto length = current_ - begin.offset;
    const std::string lexeme(source_.substr(begin.offset, length));
    return Token{kind, lexeme, lexeme, SourceSpan{begin, location()}};
}

[[noreturn]] void Lexer::fail(std::string message, SourceLocation begin) const {
    throw SqlError(SqlErrorKind::Lexer, std::move(message), SourceSpan{begin, location()});
}

void Lexer::skipIgnored() {
    while (!atEnd()) {
        const char value = peek();
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
            static_cast<void>(advance());
            continue;
        }
        if (value == '-' && peek(1) == '-') {
            static_cast<void>(advance());
            static_cast<void>(advance());
            while (!atEnd() && peek() != '\r' && peek() != '\n') {
                static_cast<void>(advance());
            }
            continue;
        }
        if (value == '/' && peek(1) == '*') {
            const auto begin = location();
            static_cast<void>(advance());
            static_cast<void>(advance());
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) {
                static_cast<void>(advance());
            }
            if (atEnd()) {
                fail("unterminated block comment", begin);
            }
            static_cast<void>(advance());
            static_cast<void>(advance());
            continue;
        }
        break;
    }
}

Token Lexer::scanIdentifier(SourceLocation begin) {
    while (isIdentifierContinue(peek())) {
        static_cast<void>(advance());
    }
    auto token = makeToken(
        keywordKind(source_.substr(begin.offset, current_ - begin.offset)), begin);
    return token;
}

Token Lexer::scanInteger(SourceLocation begin) {
    while (isDigit(peek())) {
        static_cast<void>(advance());
    }
    if (current_ - begin.offset > MAX_INTEGER_DIGITS) {
        fail("integer literal exceeds the 1024-digit lexical limit", begin);
    }
    return makeToken(TokenKind::Integer, begin);
}

Token Lexer::scanString(SourceLocation begin) {
    std::string decoded;
    while (!atEnd()) {
        const char value = advance();
        if (value != '\'') {
            decoded.push_back(value);
            continue;
        }
        if (peek() == '\'') {
            static_cast<void>(advance());
            decoded.push_back('\'');
            continue;
        }
        auto token = makeToken(TokenKind::String, begin);
        token.value = std::move(decoded);
        return token;
    }
    fail("unterminated string literal", begin);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skipIgnored();
        if (atEnd()) {
            const auto end = location();
            tokens.push_back(Token{TokenKind::EndOfFile, "", "", SourceSpan{end, end}});
            return tokens;
        }

        const auto begin = location();
        const char value = advance();
        if (isIdentifierStart(value)) {
            tokens.push_back(scanIdentifier(begin));
            continue;
        }
        if (isDigit(value)) {
            tokens.push_back(scanInteger(begin));
            continue;
        }
        switch (value) {
        case '\'': tokens.push_back(scanString(begin)); break;
        case '(': tokens.push_back(makeToken(TokenKind::LeftParen, begin)); break;
        case ')': tokens.push_back(makeToken(TokenKind::RightParen, begin)); break;
        case ',': tokens.push_back(makeToken(TokenKind::Comma, begin)); break;
        case ';': tokens.push_back(makeToken(TokenKind::Semicolon, begin)); break;
        case '*': tokens.push_back(makeToken(TokenKind::Star, begin)); break;
        case '=': tokens.push_back(makeToken(TokenKind::Equal, begin)); break;
        case '-': tokens.push_back(makeToken(TokenKind::Minus, begin)); break;
        case '!':
            if (!match('=')) {
                fail("unexpected '!'; only '!=' is supported", begin);
            }
            tokens.push_back(makeToken(TokenKind::BangEqual, begin));
            break;
        case '<':
            if (match('=')) {
                tokens.push_back(makeToken(TokenKind::LessEqual, begin));
            } else if (match('>')) {
                tokens.push_back(makeToken(TokenKind::LessGreater, begin));
            } else {
                tokens.push_back(makeToken(TokenKind::Less, begin));
            }
            break;
        case '>':
            tokens.push_back(makeToken(
                match('=') ? TokenKind::GreaterEqual : TokenKind::Greater, begin));
            break;
        default:
            fail("unexpected character", begin);
        }
    }
}

} // namespace minidb::sql
