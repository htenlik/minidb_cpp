#pragma once

#include "minidb/row.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minidb::test {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view testName)
        : path_(std::filesystem::temp_directory_path()
                / ("minidb_cpp_" + std::string(testName) + "_"
                   + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count())
                   + ".db")) {}

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Exception, typename Function>
void requireThrows(Function&& function, std::string_view failureMessage) {
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string(failureMessage) + " Wrong exception: " + error.what());
    }
    throw std::runtime_error(std::string(failureMessage));
}

inline Row makeRow(std::uint32_t id) {
    return Row{
        id,
        "user" + std::to_string(id),
        "user" + std::to_string(id) + "@example.com",
    };
}

} // namespace minidb::test
