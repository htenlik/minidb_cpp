#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>

int main() {
    try {
        minidb::bench::BenchmarkConfig config;
        config.suite = "quick";
        config.rows = 12;
        config.operations = 6;
        config.pages = 10;
        config.warmupOperations = 2;
        config.seed = 12345;
        config.databasePath = (std::filesystem::temp_directory_path()
            / "minidb_benchmark_quick_suite.db").string();
        const auto results = minidb::bench::runConfiguredBenchmarks(config);
        std::set<std::string> names;
        for (const auto& result : results) {
            minidb::test::require(
                result.validationPassed && result.timing.operationCount == 6,
                "quick-suite workload failed validation or operation count");
            names.insert(result.benchmark);
        }
        minidb::test::require(
            names == std::set<std::string>{
                "pager_sequential", "pager_random", "buffer_random", "bplus_find_hit",
                "tuple_lookup",
                "sql_pk_lookup", "sql_heap_scan", "sql_mixed", "tcp_pk_lookup",
            },
            "quick-suite workload membership changed");
        const auto json = minidb::bench::resultsToJson(results);
        minidb::test::require(
            json.find("\"schema_version\":1") != std::string::npos
                && json.find("\"benchmark\":\"tcp_pk_lookup\"") != std::string::npos,
            "quick-suite JSON was incomplete");
        std::cout << "benchmark quick suite tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark quick suite test failure: " << error.what() << '\n';
        return 1;
    }
}
