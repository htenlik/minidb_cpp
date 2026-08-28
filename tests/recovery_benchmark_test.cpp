#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>

int main() {
    try {
        for (const auto& name : {std::string("txn_insert"),
                                 std::string("recovery_full_scan"),
                                 std::string("recovery_loser")}) {
            for (const auto mode : {minidb::WalUpdateMode::FullPage,
                                    minidb::WalUpdateMode::ByteRange,
                                    minidb::WalUpdateMode::Adaptive}) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = name;
            config.rows = 8;
            config.operations = 6;
            config.bufferFrames = 4;
            config.walUpdateMode = mode;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_recovery_benchmark_" + name + ".db")).string();
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            minidb::test::require(results.size() == 1 && results[0].validationPassed,
                                  "Recovery benchmark did not validate");
            if (name == "txn_insert") {
                minidb::test::require(
                    results[0].recovery.transactions.transactionsCommitted == 6
                        && results[0].recovery.transactions.pageUpdateRecords > 0
                        && results[0].recovery.transactions.updateRecordCount > 0
                        && results[0].recovery.transactions.logicalBytesChanged > 0
                        && results[0].recovery.transactions.walUpdatePayloadBytes > 0
                        && results[0].recovery.updateRecordBytes.count > 0
                        && results[0].recovery.walBytes > 0
                        && results[0].recovery.transactions.walTotalBytesGenerated
                            == results[0].recovery.walBytes
                        && results[0].recovery.loggingAmplification > 1.0,
                    "Transaction benchmark omitted WAL/amplification diagnostics");
                if (mode == minidb::WalUpdateMode::ByteRange) {
                    minidb::test::require(
                        results[0].recovery.transactions.byteRangeUpdateRecords
                                == results[0].recovery.transactions.updateRecordCount
                            && results[0].recovery.rangesPerDelta.count
                                == results[0].recovery.transactions.updateRecordCount,
                        "Byte-range benchmark omitted range/diff diagnostics");
                } else if (mode == minidb::WalUpdateMode::FullPage) {
                    minidb::test::require(
                        results[0].recovery.transactions.fullPageUpdateRecords
                                == results[0].recovery.transactions.updateRecordCount
                            && results[0].recovery.rangesPerDelta.count == 0,
                        "Full-page benchmark reported byte-range diagnostics");
                } else {
                    minidb::test::require(
                        results[0].recovery.transactions.adaptiveFullPageSelections
                                + results[0].recovery.transactions.adaptiveDeltaSelections
                            == results[0].recovery.transactions.updateRecordCount
                            && results[0].recovery.transactions.bytesActuallyChosen
                                <= results[0].recovery.transactions.bytesIfFullPage
                            && results[0].recovery.transactions.bytesActuallyChosen
                                <= results[0].recovery.transactions.bytesIfDelta,
                        "Adaptive benchmark omitted minimum-size diagnostics");
                }
            } else {
                minidb::test::require(
                    results[0].recovery.recovery.recordsAnalyzed > 0
                        && results[0].recovery.recovery.pagesRedone > 0
                        && (name != "recovery_loser"
                            || (results[0].recovery.recovery.loserTransactions == 1
                                && results[0].recovery.recovery.pagesTruncated > 0))
                        && results[0].timing.operationCount == 1,
                    "Recovery benchmark omitted analysis/REDO metrics");
            }
            const auto json = minidb::bench::resultsToJson(results);
            minidb::test::require(
                json.find("\"logging_amplification\"") != std::string::npos
                    && json.find("\"analysis_ns\"") != std::string::npos,
                "Recovery benchmark JSON omitted diagnostics");
            }
        }
        for (const auto& name : {std::string("txn_wal_delta_friendly"),
                                 std::string("txn_wal_fragmentation")}) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = name;
            config.operations = 4;
            config.bufferFrames = 2;
            config.walUpdateMode = minidb::WalUpdateMode::Adaptive;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_" + name + ".db")).string();
            const auto result = minidb::bench::runConfiguredBenchmarks(config).front();
            const auto& stats = result.recovery.transactions;
            minidb::test::require(
                result.validationPassed && stats.updateRecordCount == config.operations
                    && stats.bytesActuallyChosen <= stats.bytesIfFullPage
                    && stats.bytesActuallyChosen <= stats.bytesIfDelta,
                "Adaptive controlled workload violated minimum-size selection");
            if (name == "txn_wal_delta_friendly") {
                minidb::test::require(
                    stats.adaptiveDeltaSelections == config.operations
                        && stats.adaptiveFullPageSelections == 0,
                    "Delta-friendly workload did not select delta consistently");
            } else {
                minidb::test::require(
                    stats.adaptiveFullPageSelections == config.operations
                        && stats.adaptiveDeltaSelections == 0,
                    "Fragmentation workload did not select full-page consistently");
            }
        }
        for (const auto mode : {minidb::WalUpdateMode::FullPage,
                                minidb::WalUpdateMode::ByteRange,
                                minidb::WalUpdateMode::Adaptive}) {
            for (const auto persistedPercent : {0U, 50U, 100U}) {
                minidb::bench::BenchmarkConfig config;
                config.benchmark = "recovery_page_lsn_compare";
                config.operations = 12;
                config.bufferFrames = 2;
                config.walUpdateMode = mode;
                config.redoPersistedPercent = persistedPercent;
                config.databasePath = (std::filesystem::temp_directory_path()
                    / ("minidb_page_lsn_benchmark_"
                       + std::to_string(static_cast<unsigned>(mode)) + "_"
                       + std::to_string(persistedPercent) + ".db")).string();
                const auto result = minidb::bench::runConfiguredBenchmarks(config).front();
                const auto persisted = config.operations * persistedPercent / 100U;
                minidb::test::require(
                    result.validationPassed
                        && result.recovery.recovery.pageLsnChecks == config.operations
                        && result.recovery.recovery.redoSkippedByPageLsn == persisted
                        && result.recovery.recovery.pagesRedone
                            == config.operations - persisted
                        && result.recovery.fullScanRecovery.pagesRedone == config.operations,
                    "Selective PageLSN benchmark comparison was inconsistent");
                const auto json = minidb::bench::resultsToJson({result});
                minidb::test::require(
                    json.find("\"redo_skipped_by_page_lsn\"") != std::string::npos
                        && json.find("\"always_redo_pages\"") != std::string::npos,
                    "PageLSN benchmark JSON omitted comparison counters");
            }
        }
        std::cout << "recovery_benchmark_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_benchmark_test failed: " << error.what() << '\n';
        return 1;
    }
}
