#pragma once

#include "minidb/buffer_pool_types.hpp"
#include "minidb/database_format.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace minidb {

class BufferPoolManager;

class BasicPageGuard {
public:
    BasicPageGuard() = default;
    ~BasicPageGuard();

    BasicPageGuard(const BasicPageGuard&) = delete;
    BasicPageGuard& operator=(const BasicPageGuard&) = delete;
    BasicPageGuard(BasicPageGuard&& other) noexcept;
    BasicPageGuard& operator=(BasicPageGuard&& other);

    void drop();
    [[nodiscard]] bool isValid() const noexcept { return manager_ != nullptr; }
    [[nodiscard]] PageId pageId() const noexcept { return pageId_; }
    [[nodiscard]] FrameId frameId() const noexcept { return frameId_; }
    [[nodiscard]] std::span<const std::byte, database_format::PAGE_SIZE> data() const;

private:
    friend class BufferPoolManager;
    friend class WritePageGuard;

    BasicPageGuard(BufferPoolManager& manager, FrameId frameId, PageId pageId) noexcept;
    [[nodiscard]] std::span<std::byte, database_format::PAGE_SIZE> mutableData();

    BufferPoolManager* manager_ = nullptr;
    FrameId frameId_ = INVALID_FRAME_ID;
    PageId pageId_ = INVALID_PAGE_ID;
};

class ReadPageGuard {
public:
    ReadPageGuard() = default;
    ~ReadPageGuard() = default;
    ReadPageGuard(const ReadPageGuard&) = delete;
    ReadPageGuard& operator=(const ReadPageGuard&) = delete;
    ReadPageGuard(ReadPageGuard&&) noexcept = default;
    ReadPageGuard& operator=(ReadPageGuard&&) = default;

    void drop() { guard_.drop(); }
    [[nodiscard]] bool isValid() const noexcept { return guard_.isValid(); }
    [[nodiscard]] PageId pageId() const noexcept { return guard_.pageId(); }
    [[nodiscard]] FrameId frameId() const noexcept { return guard_.frameId(); }
    [[nodiscard]] std::span<const std::byte, database_format::PAGE_SIZE> data() const {
        return guard_.data();
    }

private:
    friend class BufferPoolManager;
    explicit ReadPageGuard(BasicPageGuard&& guard) noexcept : guard_(std::move(guard)) {}
    BasicPageGuard guard_;
};

class WritePageGuard {
public:
    WritePageGuard() = default;
    ~WritePageGuard() = default;
    WritePageGuard(const WritePageGuard&) = delete;
    WritePageGuard& operator=(const WritePageGuard&) = delete;
    WritePageGuard(WritePageGuard&&) noexcept = default;
    WritePageGuard& operator=(WritePageGuard&&) = default;

    void drop() { guard_.drop(); }
    [[nodiscard]] bool isValid() const noexcept { return guard_.isValid(); }
    [[nodiscard]] PageId pageId() const noexcept { return guard_.pageId(); }
    [[nodiscard]] FrameId frameId() const noexcept { return guard_.frameId(); }
    [[nodiscard]] std::span<const std::byte, database_format::PAGE_SIZE> data() const {
        return guard_.data();
    }
    [[nodiscard]] std::span<std::byte, database_format::PAGE_SIZE> data() {
        return guard_.mutableData();
    }

private:
    friend class BufferPoolManager;
    explicit WritePageGuard(BasicPageGuard&& guard) noexcept : guard_(std::move(guard)) {}
    BasicPageGuard guard_;
};

} // namespace minidb
