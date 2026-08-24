#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace minidb::bench;

template <typename Function>
void reject(Function&& function, std::string_view message) {
    minidb::test::requireThrows<std::invalid_argument>(
        std::forward<Function>(function), message);
}

void testPercentilesAndAccumulation() {
    minidb::test::require(nearestRankPercentile({42}, 50) == 42,
                          "single-sample percentile changed");
    minidb::test::require(nearestRankPercentile({4, 1, 3, 2}, 50) == 2
                              && nearestRankPercentile({4, 1, 3, 2}, 95) == 4
                              && nearestRankPercentile({4, 1, 3, 2}, 99) == 4,
                          "nearest-rank percentile semantics changed");
    reject([] { static_cast<void>(nearestRankPercentile({}, 50)); },
           "empty percentile sample was accepted");
    reject([] { static_cast<void>(nearestRankPercentile({1}, 0)); },
           "zero percentile was accepted");

    const auto timing = summarizeTimings({10, 20, 30, 40}, 100);
    minidb::test::require(
        timing.operationCount == 4 && timing.totalNanoseconds == 100
            && timing.meanNanoseconds == 25.0 && timing.p50Nanoseconds == 20
            && timing.p95Nanoseconds == 40 && timing.minimumNanoseconds == 10
            && timing.maximumNanoseconds == 40,
        "timing summary was incorrect");

    minidb::PagerStats first{1, 2, 3, 4, 5, 6, 7, 8, 9};
    minidb::PagerStats second{10, 20, 30, 40, 50, 60, 70, 80, 5};
    const auto total = accumulatePagerStats(first, second);
    minidb::test::require(
        total.pageRequests == 11 && total.cacheHits == 22
            && total.cacheMisses == 33 && total.physicalPageReads == 44
            && total.physicalPageWrites == 55 && total.dirtyMarks == 66
            && total.flushCalls == 77 && total.appendedPages == 88
            && total.residentPages == 9,
        "Pager statistics accumulation was incorrect");

    minidb::BufferPoolStats bufferFirst{};
    bufferFirst.pageRequests = 2;
    bufferFirst.cacheHits = 1;
    bufferFirst.evictions = 3;
    bufferFirst.residentPages = 4;
    bufferFirst.capacity = 8;
    minidb::BufferPoolStats bufferSecond{};
    bufferSecond.pageRequests = 5;
    bufferSecond.cacheHits = 4;
    bufferSecond.evictions = 6;
    bufferSecond.residentPages = 2;
    bufferSecond.capacity = 8;
    const auto bufferTotal = accumulateBufferStats(bufferFirst, bufferSecond);
    minidb::test::require(
        bufferTotal.pageRequests == 7 && bufferTotal.cacheHits == 5
            && bufferTotal.evictions == 9 && bufferTotal.residentPages == 4
            && bufferTotal.capacity == 8,
        "BufferPool statistics accumulation was incorrect");
}

void testConfigurationParsing() {
    const std::vector<std::string_view> arguments{
        "--benchmark", "pager_random", "--rows", "17", "--operations", "23",
        "--pages", "19", "--working-set", "7", "--warmup", "3",
        "--reopen-interval", "5", "--buffer-frames", "11", "--lru-k", "3",
        "--seed", "99", "--repetitions", "2",
        "--db", "bench.db", "--json", "out.json", "--tuple-sizes", "medium",
        "--mode", "reopen", "--retain-db",
    };
    const auto config = parseArguments(arguments).config;
    minidb::test::require(
        config.benchmark == "pager_random" && config.rows == 17
            && config.operations == 23 && config.pages == 19
            && config.workingSet == 7 && config.warmupOperations == 3
            && config.reopenInterval == 5 && config.seed == 99
            && config.bufferFrames == 11 && config.lruK == 3
            && config.repetitions == 2 && config.databasePath == "bench.db"
            && config.jsonPath == "out.json" && config.tupleSizes == "medium"
            && config.cacheMode == CacheMode::Reopen && config.keepDatabase,
        "benchmark configuration parsing changed");

    reject([] { static_cast<void>(parseArguments(std::vector<std::string_view>{})); },
           "missing benchmark/suite was accepted");
    reject([] { static_cast<void>(parseArguments(
        std::vector<std::string_view>{"--benchmark", "pager", "--operations", "0"})); },
        "zero operation count was accepted");
    reject([] { static_cast<void>(parseArguments(
        std::vector<std::string_view>{"--benchmark", "buffer", "--buffer-frames", "0"})); },
        "zero buffer capacity was accepted");
    reject([] { static_cast<void>(parseArguments(
        std::vector<std::string_view>{"--benchmark", "buffer", "--lru-k", "0"})); },
        "zero LRU-K K was accepted");
    reject([] { static_cast<void>(parseArguments(
        std::vector<std::string_view>{"--benchmark", "missing"})); },
        "unknown benchmark was accepted");
    reject([] { static_cast<void>(parseArguments(
        std::vector<std::string_view>{"--suite", "slow"})); },
        "unknown suite was accepted");
    reject([] { static_cast<void>(parseArguments(
        std::vector<std::string_view>{"--benchmark", "pager", "--suite", "quick"})); },
        "benchmark and suite together were accepted");
}

void testJson() {
    minidb::test::require(
        escapeJson("a\n\"b\\\t") == "a\\n\\\"b\\\\\\t",
        "JSON escaping changed");
    BenchmarkResult result;
    result.benchmark = "demo\nname";
    result.seed = 7;
    result.repetition = 1;
    result.configuration.benchmark = "demo";
    result.configuration.rows = 1;
    result.configuration.operations = 1;
    result.configuration.reopenInterval = 17;
    result.configuration.repetitions = 3;
    result.timing = summarizeTimings({10}, 10);
    result.pager = minidb::PagerStats{1, 2, 3, 4, 5, 6, 7, 8, 9};
    result.buffer.pageRequests = 10;
    result.buffer.cacheHits = 7;
    result.buffer.capacity = 4;
    result.storageBefore = StorageMetrics{10, 40'960, 2, 3};
    result.storageAfter = StorageMetrics{12, 49'152, 1, 7};
    result.environment = currentEnvironment();
    result.validationPassed = true;
    const auto json = resultsToJson({result});
    minidb::test::require(
        json.find("\"schema_version\":1") != std::string::npos
            && json.find("demo\\nname") != std::string::npos
            && json.find("\"reopen_interval\":17") != std::string::npos
            && json.find("\"repetitions\":3") != std::string::npos
            && json.find("\"dirty_marks\":6") != std::string::npos
            && json.find("\"hit_ratio\":0.7") != std::string::npos
            && json.find("\"capacity\":4") != std::string::npos
            && json.find("\"before\":{\"database_pages\":10") != std::string::npos
            && json.find("\"after\":{\"database_pages\":12") != std::string::npos
            && json.find("\"validation_passed\":true") != std::string::npos
            && json.back() == '\n',
        "benchmark JSON schema/output changed");
}

} // namespace

int main() {
    try {
        testPercentilesAndAccumulation();
        testConfigurationParsing();
        testJson();
        std::cout << "benchmark utility tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark utility test failure: " << error.what() << '\n';
        return 1;
    }
}
