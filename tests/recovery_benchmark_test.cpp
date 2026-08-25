#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>

int main() {
    try {
        for (const auto& name : {std::string("txn_insert"), std::string("recovery_full_scan")}) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = name;
            config.rows = 8;
            config.operations = 6;
            config.bufferFrames = 4;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_recovery_benchmark_" + name + ".db")).string();
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            minidb::test::require(results.size() == 1 && results[0].validationPassed,
                                  "Recovery benchmark did not validate");
            if (name == "txn_insert") {
                minidb::test::require(
                    results[0].recovery.transactions.transactionsCommitted == 6
                        && results[0].recovery.transactions.pageUpdateRecords > 0
                        && results[0].recovery.walBytes > 0
                        && results[0].recovery.loggingAmplification > 1.0,
                    "Transaction benchmark omitted WAL/amplification diagnostics");
            } else {
                minidb::test::require(
                    results[0].recovery.recovery.recordsAnalyzed > 0
                        && results[0].recovery.recovery.pagesRedone > 0
                        && results[0].timing.operationCount == 1,
                    "Recovery benchmark omitted analysis/REDO metrics");
            }
            const auto json = minidb::bench::resultsToJson(results);
            minidb::test::require(
                json.find("\"logging_amplification\"") != std::string::npos
                    && json.find("\"analysis_ns\"") != std::string::npos,
                "Recovery benchmark JSON omitted diagnostics");
        }
        std::cout << "recovery_benchmark_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_benchmark_test failed: " << error.what() << '\n';
        return 1;
    }
}
