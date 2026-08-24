#pragma once

#include "minidb/pager.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace minidb::bench {

enum class CacheMode {
    Hot,
    Reopen,
};

struct BenchmarkConfig {
    std::string benchmark;
    std::string suite;
    std::uint64_t rows = 1'000;
    std::uint64_t operations = 1'000;
    std::uint64_t pages = 1'000;
    std::uint64_t workingSet = 0;
    std::uint64_t warmupOperations = 100;
    std::uint64_t reopenInterval = 250;
    std::uint64_t seed = 12'345;
    std::uint32_t repetitions = 1;
    std::string databasePath = "minidb_benchmark.db";
    std::string jsonPath;
    std::string tupleSizes = "mixed";
    CacheMode cacheMode = CacheMode::Hot;
    bool keepDatabase = false;
};

struct ParseResult {
    BenchmarkConfig config;
    bool helpRequested = false;
};

struct TimingMetrics {
    std::uint64_t operationCount = 0;
    std::uint64_t totalNanoseconds = 0;
    double operationsPerSecond = 0.0;
    double meanNanoseconds = 0.0;
    std::uint64_t p50Nanoseconds = 0;
    std::uint64_t p95Nanoseconds = 0;
    std::uint64_t p99Nanoseconds = 0;
    std::uint64_t minimumNanoseconds = 0;
    std::uint64_t maximumNanoseconds = 0;
};

struct StorageMetrics {
    std::uint64_t databasePages = 0;
    std::uint64_t databaseBytes = 0;
    std::uint64_t freePages = 0;
    std::uint64_t residentPages = 0;
};

struct EnvironmentMetadata {
    std::string versionContext;
    std::string gitCommit;
    std::string compiler;
    std::string buildType;
    std::string platform;
    std::uint64_t cppStandard = 0;
    std::uint64_t pageSize = 0;
    std::uint64_t protocolVersion = 0;
    std::uint64_t hardwareConcurrency = 0;
};

struct BenchmarkResult {
    std::string benchmark;
    std::uint64_t seed = 0;
    std::uint32_t repetition = 0;
    BenchmarkConfig configuration;
    TimingMetrics timing;
    PagerStats pager;
    StorageMetrics storageBefore;
    StorageMetrics storageAfter;
    double averageRowsExamined = 0.0;
    double averageIndexLookups = 0.0;
    EnvironmentMetadata environment;
    bool validationPassed = false;
};

[[nodiscard]] ParseResult parseArguments(std::span<const std::string_view> arguments);
[[nodiscard]] std::string usageText();
[[nodiscard]] std::vector<std::string> supportedBenchmarkNames();

// Nearest rank: sort ascending, rank=ceil(percent*N), clamp to [1,N], return rank-1.
[[nodiscard]] std::uint64_t nearestRankPercentile(
    std::vector<std::uint64_t> samples,
    unsigned percent);
[[nodiscard]] TimingMetrics summarizeTimings(
    const std::vector<std::uint64_t>& latenciesNanoseconds,
    std::uint64_t totalNanoseconds);
[[nodiscard]] PagerStats accumulatePagerStats(PagerStats accumulated, const PagerStats& next);
[[nodiscard]] std::string escapeJson(std::string_view value);
[[nodiscard]] std::string resultsToJson(const std::vector<BenchmarkResult>& results);
[[nodiscard]] std::string formatHuman(const BenchmarkResult& result);
[[nodiscard]] EnvironmentMetadata currentEnvironment();

[[nodiscard]] std::vector<BenchmarkResult> runConfiguredBenchmarks(
    const BenchmarkConfig& configuration);

} // namespace minidb::bench
