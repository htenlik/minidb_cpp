#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
namespace minidb {
class Pager {
public:
    static constexpr std::size_t PAGE_SIZE = 4096;
    using Page = std::array<std::byte, PAGE_SIZE>;
    explicit Pager(const std::string& path);
    ~Pager();
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;
    Page& getPage(std::uint32_t pageId);
    std::uint32_t allocatePage();
    void markDirty(std::uint32_t pageId);
    void flush(std::uint32_t pageId);
    void flushAll();
    [[nodiscard]] std::uint32_t pageCount() const noexcept { return pageCount_; }
private:
    struct Frame { Page data{}; bool dirty = false; };
    std::string path_;
    std::fstream file_;
    std::uint32_t pageCount_ = 0;
    std::unordered_map<std::uint32_t, std::unique_ptr<Frame>> cache_;
    void openOrCreate();
    void loadPageFromDisk(std::uint32_t pageId, Frame& frame);
};
} // namespace minidb
