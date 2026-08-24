#include "minidb/cli_format.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    try {
        const minidb::sql::QueryResult selection = minidb::sql::SelectResult{
            {"id", "name", "active"},
            {{1U, std::string("alice"), true},
             {2U, std::monostate{}, false}},
            {},
            {minidb::sql::AccessPath::PrimaryKeyLookup, 2, 2, 1},
        };
        const auto formatted = minidb::cli::formatQueryResult(selection, true);
        minidb::test::require(
            formatted.find("id | name  | active") != std::string::npos
                && formatted.find("2  | NULL  | false") != std::string::npos
                && formatted.find("access: PrimaryKeyLookup") != std::string::npos
                && formatted.find("index lookups: 1") != std::string::npos,
            "CLI SELECT table/NULL/stats formatting changed");

        const minidb::sql::QueryResult command = minidb::sql::CommandResult{
            minidb::sql::CommandKind::Update, 2, std::nullopt, {},
        };
        minidb::test::require(
            minidb::cli::formatQueryResult(command) == "UPDATE\n2 rows affected\n",
            "CLI command formatting changed");

        minidb::test::requireThrows<std::invalid_argument>(
            [] { static_cast<void>(minidb::cli::formatQueryResult(
                minidb::sql::SelectResult{{"one"}, {{1U, 2U}}, {}, {}})); },
            "malformed programmatic SELECT result caused unsafe formatting");

        std::cout << "CLI format tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CLI format test failure: " << error.what() << '\n';
        return 1;
    }
}
