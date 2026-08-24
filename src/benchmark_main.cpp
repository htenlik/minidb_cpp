#include "minidb/benchmark.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    try {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        const auto parsed = minidb::bench::parseArguments(arguments);
        if (parsed.helpRequested) {
            std::cout << minidb::bench::usageText();
            return 0;
        }

        const auto results = minidb::bench::runConfiguredBenchmarks(parsed.config);
        for (const auto& result : results) {
            std::cout << minidb::bench::formatHuman(result) << '\n';
        }
        if (!parsed.config.jsonPath.empty()) {
            const std::filesystem::path outputPath(parsed.config.jsonPath);
            if (outputPath.has_parent_path()) {
                std::filesystem::create_directories(outputPath.parent_path());
            }
            std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("could not open benchmark JSON output");
            output << minidb::bench::resultsToJson(results);
            if (!output) throw std::runtime_error("could not write benchmark JSON output");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_bench: " << error.what() << '\n';
        return 1;
    }
}
