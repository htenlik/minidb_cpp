#pragma once

#include "minidb/sql_source.hpp"

#include <string>
#include <string_view>

namespace minidb::sql {

enum class TokenKind {
    EndOfFile,
    Identifier,
    Integer,
    String,

    Create,
    Table,
    Insert,
    Into,
    Values,
    Select,
    From,
    Where,
    Update,
    Set,
    Delete,
    Primary,
    Key,
    Not,
    Null,
    Uint32,
    Int64,
    Boolean,
    Varchar,
    True,
    False,
    And,
    Or,

    LeftParen,
    RightParen,
    Comma,
    Semicolon,
    Star,
    Equal,
    BangEqual,
    LessGreater,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Minus,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string lexeme;
    std::string value;
    SourceSpan span{};

    bool operator==(const Token&) const = default;
};

[[nodiscard]] std::string_view tokenKindName(TokenKind kind) noexcept;

} // namespace minidb::sql
