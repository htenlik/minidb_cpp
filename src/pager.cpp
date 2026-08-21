#include "minidb/pager.hpp"
#include <filesystem>
#include <stdexcept>
namespace minidb {
Pager::Pager(const std::string& path) : path_(path) {
    openOrCreate();
    file_.seekg(0, std::ios::end);
    const auto size = file_.tellg();
    if (size < 0) throw std::runtime_error("Could not determine database file size.");
    const auto bytes = static_cast<std::uint64_t>(size);
    if (bytes % PAGE_SIZE != 0)
        throw std::runtime_error("Database file is corrupted: file size is not a multiple of PAGE_SIZE.");
    pageCount_ = static_cast<std::uint32_t>(bytes / PAGE_SIZE);
}
Pager::~Pager() { try { flushAll(); } catch (...) {} }
void Pager::openOrCreate() {
    if (!std::filesystem::exists(path_)) {
        std::ofstream creator(path_, std::ios::binary);
        if (!creator) throw std::runtime_error("Could not create database file: " + path_);
    }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw std::runtime_error("Could not open database file: " + path_);
}
void Pager::loadPageFromDisk(std::uint32_t pageId, Frame& frame) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);
    file_.read(reinterpret_cast<char*>(frame.data.data()), static_cast<std::streamsize>(PAGE_SIZE));
    if (file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE))
        throw std::runtime_error("Failed to read full page from disk.");
}
Pager::Page& Pager::getPage(std::uint32_t pageId) {
    if (pageId >= pageCount_) throw std::out_of_range("Page ID does not exist.");
    if (auto it = cache_.find(pageId); it != cache_.end()) return it->second->data;
    auto frame = std::make_unique<Frame>();
    loadPageFromDisk(pageId, *frame);
    auto& page = frame->data;
    cache_.emplace(pageId, std::move(frame));
    return page;
}
std::uint32_t Pager::allocatePage() {
    const std::uint32_t pageId = pageCount_++;
    auto frame = std::make_unique<Frame>();
    frame->data.fill(std::byte{0});
    frame->dirty = true;
    cache_.emplace(pageId, std::move(frame));
    return pageId;
}
void Pager::markDirty(std::uint32_t pageId) {
    auto it = cache_.find(pageId);
    if (it == cache_.end()) throw std::runtime_error("Cannot mark a page dirty before loading it.");
    it->second->dirty = true;
}
void Pager::flush(std::uint32_t pageId) {
    auto it = cache_.find(pageId);
    if (it == cache_.end() || !it->second->dirty) return;
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(PAGE_SIZE), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(it->second->data.data()), static_cast<std::streamsize>(PAGE_SIZE));
    if (!file_) throw std::runtime_error("Failed to write page to disk.");
    file_.flush();
    it->second->dirty = false;
}
void Pager::flushAll() { for (const auto& [pageId, _] : cache_) flush(pageId); }
} // namespace minidb
