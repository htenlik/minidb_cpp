#pragma once

#include "minidb/row.hpp"
#include "minidb/buffer_pool_manager.hpp"
#include "minidb/disk_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_access.hpp"

#include <algorithm>
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
        std::filesystem::remove(path_.string() + ".wal", error);
        std::filesystem::remove(path_.string() + ".ckpt", error);
        std::filesystem::remove_all(path_.string() + ".wal.d", error);
        std::filesystem::remove_all(path_.string() + ".wal.d.tmp", error);
    }

    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class TestStorage {
public:
    explicit TestStorage(
        const std::filesystem::path& path,
        std::size_t bufferFrames = 128,
        std::size_t lruK = 2)
        : diskManager(path.string()),
          bufferPool(diskManager, bufferFrames, lruK),
          allocator(bufferPool, diskManager) {}

    DiskManager diskManager;
    BufferPoolManager bufferPool;
    PageAllocator allocator;
};

inline DiskManager::Page readPageCopy(TestStorage& storage, PageId pageId) {
    const auto guard = requireReadPage(storage.bufferPool, pageId, "copy test page");
    DiskManager::Page result{};
    std::copy(guard.data().begin(), guard.data().end(), result.begin());
    return result;
}

template <typename Function>
void mutatePage(TestStorage& storage, PageId pageId, Function&& function) {
    auto guard = requireWritePage(storage.bufferPool, pageId, "mutate test page");
    auto bytes = guard.data();
    function(bytes);
}

inline void requireBufferClean(TestStorage& storage) {
    storage.bufferPool.validate();
    storage.bufferPool.validateReplacer();
    if (storage.bufferPool.stats().pinnedFrames != 0) {
        throw std::runtime_error("Completed operation leaked a buffer-frame pin");
    }
}

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
