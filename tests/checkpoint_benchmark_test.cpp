#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <iostream>

int main() {
    try {
        minidb::bench::BenchmarkConfig config;
        config.benchmark = "checkpoint_latency";
        config.rows = 8;
        config.operations = 4;
        config.bufferFrames = 16;
        config.databasePath = (std::filesystem::temp_directory_path()
            / "minidb_checkpoint_benchmark_test.db").string();
        auto results = minidb::bench::runConfiguredBenchmarks(config);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].checkpoint.checkpointsCompleted == 1
                    && results[0].timing.operationCount == 1,
                "Checkpoint latency benchmark smoke result is invalid");

        config.checkpointMode = minidb::CheckpointMode::Fuzzy;
        results = minidb::bench::runConfiguredBenchmarks(config);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].checkpoint.fuzzyCheckpointsCompleted == 1
                    && results[0].checkpoint.databaseSyncs == 0
                    && results[0].checkpoint.dptEntriesCaptured != 0,
                "Fuzzy checkpoint latency benchmark smoke result is invalid");

        config.benchmark = "recovery_checkpoint_compare";
        config.operations = 12;
        config.checkpointMode = minidb::CheckpointMode::Sharp;
        results = minidb::bench::runConfiguredBenchmarks(config);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].recovery.recovery.checkpointUsed
                    && results[0].recovery.recovery.walBytesSkipped > 0
                    && results[0].recovery.recovery.recordsAnalyzed
                        < results[0].recovery.fullScanRecovery.recordsAnalyzed,
                "Checkpoint recovery comparison did not measure bounded scanning");
        config.checkpointMode = minidb::CheckpointMode::Fuzzy;
        results = minidb::bench::runConfiguredBenchmarks(config);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].recovery.recovery.checkpointMode
                        == minidb::CheckpointMode::Fuzzy
                    && results[0].recovery.recovery.checkpointDirtyPageCount != 0,
                "Fuzzy checkpoint recovery comparison did not use its DPT");
        std::cout << "checkpoint_benchmark_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "checkpoint_benchmark_test failed: " << error.what() << '\n';
        return 1;
    }
}
