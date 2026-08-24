#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
    try {
        const std::vector<std::string> families{
            "pager", "bplus", "tuple", "sql", "tcp", "mixed",
        };
        for (const auto& family : families) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = family;
            config.rows = 8;
            config.operations = 6;
            config.pages = 8;
            config.workingSet = 4;
            config.warmupOperations = 2;
            config.reopenInterval = 3;
            config.seed = 20260901;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_benchmark_smoke_" + family + ".db")).string();
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            minidb::test::require(
                results.size() == 1 && results[0].validationPassed
                    && results[0].timing.operationCount == config.operations
                    && results[0].storageAfter.databasePages > 0
                    && results[0].storageAfter.databaseBytes
                        == results[0].storageAfter.databasePages * minidb::Pager::PAGE_SIZE,
                "benchmark family smoke result was incomplete");
        }
        std::cout << "benchmark family smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark smoke test failure: " << error.what() << '\n';
        return 1;
    }
}
