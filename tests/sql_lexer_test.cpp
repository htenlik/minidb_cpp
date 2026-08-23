#include "minidb/sql_lexer.hpp"
#include "test_utils.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using minidb::sql::Lexer;
using minidb::sql::SqlError;
using minidb::sql::SqlErrorKind;
using minidb::sql::Token;
using minidb::sql::TokenKind;

std::vector<Token> lex(std::string_view source) {
    return Lexer(source).tokenize();
}

std::vector<TokenKind> kinds(const std::vector<Token>& tokens) {
    std::vector<TokenKind> result;
    result.reserve(tokens.size());
    for (const auto& token : tokens) {
        result.push_back(token.kind);
    }
    return result;
}

void testEmptyWhitespaceAndLocations() {
    const auto empty = lex("");
    minidb::test::require(empty.size() == 1 && empty.front().kind == TokenKind::EndOfFile,
                          "Empty SQL did not produce only EOF");
    minidb::test::require(empty.front().span.begin.offset == 0
                              && empty.front().span.begin.line == 1
                              && empty.front().span.begin.column == 1,
                          "Empty-input EOF location was incorrect");

    const auto tokens = lex(" \t\r\n  alpha\n beta");
    minidb::test::require(kinds(tokens) == std::vector<TokenKind>{
                              TokenKind::Identifier,
                              TokenKind::Identifier,
                              TokenKind::EndOfFile},
                          "Whitespace produced unexpected tokens");
    minidb::test::require(tokens[0].span.begin.offset == 6
                              && tokens[0].span.begin.line == 2
                              && tokens[0].span.begin.column == 3
                              && tokens[1].span.begin.line == 3
                              && tokens[1].span.begin.column == 2,
                          "Lexer line/column tracking across CRLF/LF was incorrect");
}

void testKeywordsCaseAndIdentifierBoundaries() {
    constexpr std::string_view source =
        "CREATE TABLE INSERT INTO VALUES SELECT FROM WHERE UPDATE SET DELETE "
        "PRIMARY KEY NOT NULL UINT32 INT64 BOOLEAN VARCHAR TRUE FALSE AND OR";
    const auto tokens = lex(source);
    const std::vector<TokenKind> expected{
        TokenKind::Create, TokenKind::Table, TokenKind::Insert, TokenKind::Into,
        TokenKind::Values, TokenKind::Select, TokenKind::From, TokenKind::Where,
        TokenKind::Update, TokenKind::Set, TokenKind::Delete, TokenKind::Primary,
        TokenKind::Key, TokenKind::Not, TokenKind::Null, TokenKind::Uint32,
        TokenKind::Int64, TokenKind::Boolean, TokenKind::Varchar, TokenKind::True,
        TokenKind::False, TokenKind::And, TokenKind::Or, TokenKind::EndOfFile,
    };
    minidb::test::require(kinds(tokens) == expected, "Not every SQL keyword was recognized");

    const auto mixed = lex("sElEcT selectValue _from from2 FROM");
    minidb::test::require(kinds(mixed) == std::vector<TokenKind>{
                              TokenKind::Select, TokenKind::Identifier,
                              TokenKind::Identifier, TokenKind::Identifier,
                              TokenKind::From, TokenKind::EndOfFile},
                          "Keyword case folding or identifier boundaries were incorrect");
    minidb::test::require(mixed[0].lexeme == "sElEcT"
                              && mixed[1].value == "selectValue"
                              && mixed[2].value == "_from",
                          "Lexer did not preserve source spelling");
}

void testPunctuationAndMaximalMunch() {
    const auto tokens = lex("( ) , ; * = != <> < <= > >= -");
    minidb::test::require(kinds(tokens) == std::vector<TokenKind>{
                              TokenKind::LeftParen, TokenKind::RightParen,
                              TokenKind::Comma, TokenKind::Semicolon, TokenKind::Star,
                              TokenKind::Equal, TokenKind::BangEqual,
                              TokenKind::LessGreater, TokenKind::Less,
                              TokenKind::LessEqual, TokenKind::Greater,
                              TokenKind::GreaterEqual, TokenKind::Minus,
                              TokenKind::EndOfFile},
                          "Punctuation/operator tokenization was incorrect");
    const auto adjacent = lex("<<=<>><=>=");
    minidb::test::require(kinds(adjacent) == std::vector<TokenKind>{
                              TokenKind::Less, TokenKind::LessEqual,
                              TokenKind::LessGreater, TokenKind::Greater,
                              TokenKind::LessEqual, TokenKind::GreaterEqual,
                              TokenKind::EndOfFile},
                          "Comparison operators did not use maximal munch");
}

void testIntegersAndStrings() {
    const auto numeric = lex("0 4294967295 -9223372036854775808");
    minidb::test::require(kinds(numeric) == std::vector<TokenKind>{
                              TokenKind::Integer, TokenKind::Integer, TokenKind::Minus,
                              TokenKind::Integer, TokenKind::EndOfFile},
                          "Integer/minus tokenization was incorrect");
    minidb::test::require(numeric[1].value == "4294967295"
                              && numeric[3].value == "9223372036854775808",
                          "Integer magnitude spelling was not preserved");

    const auto strings = lex("'' 'a' 'hello world' 'it''s working' '''' 'a''b''c'");
    minidb::test::require(kinds(strings) == std::vector<TokenKind>{
                              TokenKind::String, TokenKind::String, TokenKind::String,
                              TokenKind::String, TokenKind::String, TokenKind::String,
                              TokenKind::EndOfFile},
                          "String literals produced incorrect token kinds");
    minidb::test::require(strings[0].value.empty()
                              && strings[1].value == "a"
                              && strings[2].value == "hello world"
                              && strings[3].value == "it's working"
                              && strings[4].value == "'"
                              && strings[5].value == "a'b'c",
                          "SQL doubled-quote decoding was incorrect");
    const auto backslash = lex("'a\\nb'");
    minidb::test::require(backslash[0].value == "a\\nb",
                          "Lexer incorrectly treated backslash as an escape");
    const auto multiline = lex("'a\nb' next");
    minidb::test::require(multiline[0].value == "a\nb"
                              && multiline[1].span.begin.line == 2
                              && multiline[1].span.begin.column == 4,
                          "Multiline string or subsequent location was incorrect");
}

void testComments() {
    const auto tokens = lex(
        "-- leading comment\nSELECT /* middle\ncomment */ * -- tail\nFROM t");
    minidb::test::require(kinds(tokens) == std::vector<TokenKind>{
                              TokenKind::Select, TokenKind::Star, TokenKind::From,
                              TokenKind::Identifier, TokenKind::EndOfFile},
                          "Comments leaked into the token stream");
    minidb::test::require(tokens[0].span.begin.line == 2
                              && tokens[1].span.begin.line == 3
                              && tokens[2].span.begin.line == 4,
                          "Comment location accounting was incorrect");
    const auto eofComment = lex("SELECT -- comment at EOF");
    minidb::test::require(eofComment.size() == 2
                              && eofComment.back().kind == TokenKind::EndOfFile,
                          "Line comment at EOF was not skipped");
}

template <typename Function>
void requireLexerError(
    Function&& function,
    std::size_t line,
    std::size_t column,
    std::string_view message) {
    try {
        function();
    } catch (const SqlError& error) {
        minidb::test::require(error.kind() == SqlErrorKind::Lexer,
                              "Lexer failure had parser error kind");
        minidb::test::require(error.span().begin.line == line
                                  && error.span().begin.column == column,
                              "Lexer failure had incorrect source location");
        minidb::test::require(error.message().find(message) != std::string::npos,
                              "Lexer failure lacked a precise message");
        minidb::test::require(std::string(error.what()).find("line ") == 0,
                              "Formatted SQL error omitted line/column prefix");
        return;
    }
    throw std::runtime_error("Expected structured lexer error");
}

void testLexerFailuresAndLimits() {
    requireLexerError(
        [] { static_cast<void>(lex("SELECT 'unterminated")); },
        1, 8, "unterminated string");
    requireLexerError(
        [] { static_cast<void>(lex("\n/* unterminated")); },
        2, 1, "unterminated block comment");
    requireLexerError(
        [] { static_cast<void>(lex("SELECT @")); },
        1, 8, "unexpected character");
    requireLexerError(
        [] { static_cast<void>(lex("!")); },
        1, 1, "only '!='");
    requireLexerError(
        [] {
            static_cast<void>(lex(std::string(Lexer::MAX_INTEGER_DIGITS + 1, '9')));
        },
        1, 1, "1024-digit");
}

} // namespace

int main() {
    try {
        testEmptyWhitespaceAndLocations();
        testKeywordsCaseAndIdentifierBoundaries();
        testPunctuationAndMaximalMunch();
        testIntegersAndStrings();
        testComments();
        testLexerFailuresAndLimits();
        std::cout << "sql_lexer_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_lexer_test failed: " << error.what() << '\n';
        return 1;
    }
}
