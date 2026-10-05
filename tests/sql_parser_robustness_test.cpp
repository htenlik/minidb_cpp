#include "minidb/sql_error.hpp"
#include "minidb/sql_parser.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t RANDOM_SEED = 0x600DCAFEULL;
constexpr std::size_t RANDOM_INPUT_COUNT = 10000;
constexpr std::uint64_t TRANSACTION_SEED = 0x12A600DULL;
constexpr std::size_t TRANSACTION_INPUT_PAIRS = 4096;

void testGrammarCorpus() {
    const std::vector<std::string_view> corpus{
        "CREATE TABLE t (id UINT32)",
        "CREATE TABLE users (id UINT32 PRIMARY KEY, name VARCHAR(32) NOT NULL)",
        "CREATE TABLE flags (enabled BOOLEAN NULL, score INT64)",
        "INSERT INTO t VALUES (1)",
        "INSERT INTO t VALUES (-1, '', TRUE, FALSE, NULL)",
        "INSERT INTO t (id, name) VALUES (4294967295, 'it''s')",
        "SELECT * FROM t",
        "SELECT id FROM t;",
        "SELECT id, name FROM t WHERE id = 1",
        "SELECT * FROM t WHERE a != 1",
        "SELECT * FROM t WHERE a <> 1",
        "SELECT * FROM t WHERE a < 1 OR a <= 2",
        "SELECT * FROM t WHERE a > 1 AND a >= 2",
        "SELECT * FROM t WHERE NOT active = FALSE",
        "SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3",
        "SELECT * FROM nonexistent WHERE missing = NULL",
        "UPDATE t SET name = 'x'",
        "UPDATE t SET name = 'x', active = TRUE WHERE id = 1",
        "DELETE FROM t",
        "DELETE FROM t WHERE id = -9223372036854775808",
        "-- comment\nSELECT /* block */ * FROM t",
        "BEGIN",
        "BEGIN TRANSACTION;",
        "COMMIT;",
        "ROLLBACK;",
    };
    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        for (const auto source : corpus) {
            static_cast<void>(minidb::sql::Parser::parse(source));
        }
    }
}

void testRandomTransactionGrammar() {
    constexpr std::array<std::string_view, 4> statements{
        "BEGIN", "BEGIN TRANSACTION", "COMMIT", "ROLLBACK"};
    constexpr std::array<std::string_view, 4> debugNames{
        "Begin", "Begin", "Commit", "Rollback"};
    constexpr std::array<std::string_view, 8> suffixes{
        "; BEGIN", "; COMMIT", " garbage", " (", " = 1",
        " TRANSACTION TRANSACTION", " TO savepoint", "; ROLLBACK"};
    std::mt19937_64 random(TRANSACTION_SEED);
    for (std::size_t index = 0; index < TRANSACTION_INPUT_PAIRS; ++index) {
        const auto kind = static_cast<std::size_t>(random() % statements.size());
        std::string source = "/* prefix */\n";
        for (char character : statements[kind]) {
            if (character == ' ') {
                source += (random() % 2U == 0) ? " /* between */ " : "\n\t";
            } else {
                if (random() % 2U == 0) {
                    character = static_cast<char>(character - 'A' + 'a');
                }
                source.push_back(character);
            }
        }
        if (random() % 2U == 0) {
            source += ';';
        }
        source += " /* suffix */";
        const auto context = " seed=" + std::to_string(TRANSACTION_SEED)
            + " input=" + std::to_string(index) + " source=" + source;
        try {
            const auto statement = minidb::sql::Parser::parse(source);
            minidb::test::require(minidb::sql::toDebugString(statement) == debugNames[kind],
                                  "Wrong transaction AST" + context);
        } catch (const std::exception& error) {
            throw std::runtime_error("Valid transaction rejected" + context + ": " + error.what());
        }

        const auto malformed = source + std::string(suffixes[random() % suffixes.size()]);
        bool rejected = false;
        try {
            static_cast<void>(minidb::sql::Parser::parse(malformed));
        } catch (const minidb::sql::SqlError& error) {
            minidb::test::require(error.kind() == minidb::sql::SqlErrorKind::Parser,
                                  "Malformed transaction did not fail grammar validation" + context);
            rejected = true;
        }
        minidb::test::require(rejected, "Malformed transaction accepted" + context
            + " malformed=" + malformed);
    }
}

void testRandomSqlLikeInputs() {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"
        " ()',;*=!<>-/+@\n\r\t";
    std::mt19937_64 random(RANDOM_SEED);
    std::size_t validCount = 0;
    std::size_t errorCount = 0;
    for (std::size_t inputIndex = 0; inputIndex < RANDOM_INPUT_COUNT; ++inputIndex) {
        const auto length = static_cast<std::size_t>(random() % 97U);
        std::string source;
        source.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            source.push_back(alphabet[random() % alphabet.size()]);
        }
        try {
            static_cast<void>(minidb::sql::Parser::parse(source));
            ++validCount;
        } catch (const minidb::sql::SqlError&) {
            ++errorCount;
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "non-SqlError for seed=" + std::to_string(RANDOM_SEED)
                + " input=" + std::to_string(inputIndex)
                + " length=" + std::to_string(length)
                + ": " + error.what());
        }
    }
    minidb::test::require(validCount + errorCount == RANDOM_INPUT_COUNT,
                          "Robustness corpus did not process every input");
}

} // namespace

int main() {
    try {
        testGrammarCorpus();
        testRandomSqlLikeInputs();
        testRandomTransactionGrammar();
        std::cout << "sql_parser_robustness_test passed (10000 random inputs, seed 0x600DCAFE; "
                     "8192 transaction inputs, seed 0x12A600D)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_parser_robustness_test failed: " << error.what() << '\n';
        return 1;
    }
}
