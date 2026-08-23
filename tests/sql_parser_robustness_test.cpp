#include "minidb/sql_error.hpp"
#include "minidb/sql_parser.hpp"
#include "test_utils.hpp"

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
    };
    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        for (const auto source : corpus) {
            static_cast<void>(minidb::sql::Parser::parse(source));
        }
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
        std::cout << "sql_parser_robustness_test passed (10000 random inputs, seed 0x600DCAFE)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_parser_robustness_test failed: " << error.what() << '\n';
        return 1;
    }
}
