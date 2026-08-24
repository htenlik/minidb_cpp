#include "minidb/buffer_pool_manager.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using minidb::BufferPoolManager;
using minidb::DiskManager;
using minidb::FrameId;
using minidb::PageId;
using minidb::test::require;

class ReferenceBufferPool {
public:
    ReferenceBufferPool(
        std::size_t capacity,
        std::size_t k,
        std::vector<DiskManager::Page>& logical,
        std::vector<DiskManager::Page>& durable)
        : k_(k), frames_(capacity), logical_(logical), durable_(durable) {
        for (FrameId frameId = 0; frameId < capacity; ++frameId) free_.push_back(frameId);
    }

    std::optional<FrameId> fetch(PageId pageId, bool writable) {
        if (const auto found = table_.find(pageId); found != table_.end()) {
            auto& frame = frames_[found->second];
            ++frame.pinCount;
            frame.dirty = frame.dirty || writable;
            record(found->second);
            return found->second;
        }
        const auto frameId = available();
        if (!frameId.has_value()) return std::nullopt;
        install(*frameId, pageId, writable);
        return frameId;
    }

    std::optional<FrameId> newPage(PageId pageId) {
        const auto frameId = available();
        if (!frameId.has_value()) return std::nullopt;
        install(*frameId, pageId, true);
        return frameId;
    }

    void release(FrameId frameId) {
        auto& frame = frames_.at(frameId);
        if (!frame.valid || frame.pinCount == 0) throw std::logic_error("reference pin underflow");
        --frame.pinCount;
    }

    void markDirty(FrameId frameId) {
        auto& frame = frames_.at(frameId);
        if (!frame.valid || frame.pinCount == 0) {
            throw std::logic_error("reference dirty mark lacks a pin");
        }
        frame.dirty = true;
    }

    bool flushPage(PageId pageId) {
        const auto found = table_.find(pageId);
        if (found == table_.end()) return false;
        auto& frame = frames_[found->second];
        if (frame.dirty) {
            durable_[pageId] = logical_[pageId];
            frame.dirty = false;
        }
        return true;
    }

    void flushAll() {
        for (auto& frame : frames_) {
            if (frame.valid && frame.dirty) {
                durable_[frame.pageId] = logical_[frame.pageId];
                frame.dirty = false;
            }
        }
    }

    [[nodiscard]] bool resident(PageId pageId) const { return table_.contains(pageId); }
    [[nodiscard]] std::optional<FrameId> frameId(PageId pageId) const {
        const auto found = table_.find(pageId);
        return found == table_.end() ? std::nullopt
                                    : std::optional<FrameId>(found->second);
    }
    [[nodiscard]] std::optional<std::uint32_t> pins(PageId pageId) const {
        const auto id = frameId(pageId);
        return id.has_value() ? std::optional<std::uint32_t>(frames_[*id].pinCount)
                              : std::nullopt;
    }
    [[nodiscard]] std::optional<bool> dirty(PageId pageId) const {
        const auto id = frameId(pageId);
        return id.has_value() ? std::optional<bool>(frames_[*id].dirty) : std::nullopt;
    }

private:
    struct Frame {
        PageId pageId = minidb::INVALID_PAGE_ID;
        std::uint32_t pinCount = 0;
        bool dirty = false;
        bool valid = false;
        std::vector<std::uint64_t> history;
    };

    std::size_t k_;
    std::uint64_t timestamp_ = 0;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, FrameId> table_;
    std::deque<FrameId> free_;
    std::vector<DiskManager::Page>& logical_;
    std::vector<DiskManager::Page>& durable_;

    void record(FrameId frameId) {
        auto& history = frames_[frameId].history;
        history.push_back(++timestamp_);
        if (history.size() > k_) history.erase(history.begin());
    }

    std::optional<FrameId> available() const {
        if (!free_.empty()) return free_.front();
        std::optional<FrameId> best;
        for (FrameId frameId = 0; frameId < frames_.size(); ++frameId) {
            const auto& frame = frames_[frameId];
            if (!frame.valid || frame.pinCount != 0) continue;
            if (!best.has_value() || lessVictim(frameId, *best)) best = frameId;
        }
        return best;
    }

    bool lessVictim(FrameId leftId, FrameId rightId) const {
        const auto& left = frames_[leftId];
        const auto& right = frames_[rightId];
        const bool leftInfinite = left.history.size() < k_;
        const bool rightInfinite = right.history.size() < k_;
        if (leftInfinite != rightInfinite) return leftInfinite;
        if (left.history.front() != right.history.front()) {
            return left.history.front() < right.history.front();
        }
        return leftId < rightId;
    }

    void install(FrameId frameId, PageId pageId, bool dirty) {
        auto& frame = frames_[frameId];
        if (frame.valid) {
            if (frame.dirty) durable_[frame.pageId] = logical_[frame.pageId];
            table_.erase(frame.pageId);
        } else {
            require(!free_.empty() && free_.front() == frameId,
                    "reference free-frame order changed");
            free_.pop_front();
        }
        frame = {};
        frame.pageId = pageId;
        frame.pinCount = 1;
        frame.dirty = dirty;
        frame.valid = true;
        table_[pageId] = frameId;
        record(frameId);
    }
};

using Guard = std::variant<minidb::ReadPageGuard, minidb::WritePageGuard>;

struct HeldGuard {
    Guard guard;
    FrameId modelFrame;
    PageId pageId;
};

void dropGuard(HeldGuard& held, ReferenceBufferPool& model) {
    std::visit([](auto& guard) { guard.drop(); }, held.guard);
    model.release(held.modelFrame);
}

void validateAgainstModel(
    BufferPoolManager& pool,
    const ReferenceBufferPool& model,
    const std::vector<DiskManager::Page>& logical,
    std::uint64_t seed,
    std::size_t operation) {
    pool.validate();
    pool.validateReplacer();
    for (PageId pageId = 1; pageId < logical.size(); ++pageId) {
        if (pool.isResident(pageId) != model.resident(pageId)
            || pool.frameIdForPage(pageId) != model.frameId(pageId)
            || pool.pinCount(pageId) != model.pins(pageId)
            || pool.isDirty(pageId) != model.dirty(pageId)) {
            throw std::runtime_error(
                "random buffer model mismatch seed=" + std::to_string(seed)
                + " operation=" + std::to_string(operation)
                + " page=" + std::to_string(pageId)
                + " actualResident=" + std::to_string(pool.isResident(pageId))
                + " modelResident=" + std::to_string(model.resident(pageId))
                + " actualFrame=" + std::to_string(
                    pool.frameIdForPage(pageId).value_or(minidb::INVALID_FRAME_ID))
                + " modelFrame=" + std::to_string(
                    model.frameId(pageId).value_or(minidb::INVALID_FRAME_ID))
                + " actualPins=" + std::to_string(
                    pool.pinCount(pageId).value_or(std::numeric_limits<std::uint32_t>::max()))
                + " modelPins=" + std::to_string(
                    model.pins(pageId).value_or(std::numeric_limits<std::uint32_t>::max()))
                + " actualDirty=" + std::to_string(pool.isDirty(pageId).value_or(false))
                + " modelDirty=" + std::to_string(model.dirty(pageId).value_or(false)));
        }
    }
}

void runScenario(std::uint64_t seed, std::size_t capacity, std::size_t k) {
    minidb::test::TemporaryDatabase database(
        "buffer_random_" + std::to_string(seed) + "_" + std::to_string(capacity));
    std::vector<DiskManager::Page> logical(1);
    std::vector<DiskManager::Page> durable(1);
    constexpr std::size_t operationCount = 6'000;
    {
        DiskManager disk(database.path().string());
        for (std::size_t index = 0; index < 12; ++index) {
            const auto pageId = disk.appendPage();
            DiskManager::Page page{};
            page[0] = static_cast<std::byte>((seed + pageId) & 0xFFU);
            page[37] = static_cast<std::byte>((index * 11U) & 0xFFU);
            disk.writePage(pageId, page);
            logical.push_back(page);
            durable.push_back(page);
        }

        BufferPoolManager pool(disk, capacity, k);
        ReferenceBufferPool model(capacity, k, logical, durable);
        std::vector<HeldGuard> held;
        std::mt19937_64 random(seed);

        for (std::size_t operation = 0; operation < operationCount; ++operation) {
            const auto choice = random() % 100U;
            if (choice < 25U) {
                const auto pageId = static_cast<PageId>(1U + random() % (logical.size() - 1U));
                auto actual = pool.fetchPageRead(pageId);
                const auto expected = model.fetch(pageId, false);
                require(actual.has_value() == expected.has_value(),
                        "random read NoFrameAvailable result disagreed with model");
                if (actual.has_value()) {
                    require(actual->frameId() == expected && actual->data()[0] == logical[pageId][0]
                                && actual->data()[37] == logical[pageId][37],
                            "random read content/frame disagreed with model");
                    if ((random() % 4U) == 0) {
                        held.push_back({Guard(std::move(*actual)), *expected, pageId});
                    } else {
                        actual->drop();
                        model.release(*expected);
                    }
                }
            } else if (choice < 50U) {
                const auto pageId = static_cast<PageId>(1U + random() % (logical.size() - 1U));
                auto actual = pool.fetchPageWrite(pageId);
                const auto expected = model.fetch(pageId, true);
                require(actual.has_value() == expected.has_value(),
                        "random write NoFrameAvailable result disagreed with model");
                if (actual.has_value()) {
                    const auto offset = static_cast<std::size_t>(random() % DiskManager::PAGE_SIZE);
                    const auto value = static_cast<std::byte>((seed + operation + offset) & 0xFFU);
                    actual->data()[offset] = value;
                    logical[pageId][offset] = value;
                    if ((random() % 3U) == 0) {
                        held.push_back({Guard(std::move(*actual)), *expected, pageId});
                    } else {
                        actual->drop();
                        model.release(*expected);
                    }
                }
            } else if (choice < 60U) {
                const auto nextPageId = disk.pageCount();
                auto actual = pool.newPageWrite();
                const auto expected = model.newPage(nextPageId);
                require(actual.has_value() == expected.has_value(),
                        "random new-page result disagreed with model");
                if (actual.has_value()) {
                    require(actual->pageId() == nextPageId && actual->frameId() == expected,
                            "random new page identity disagreed with model");
                    DiskManager::Page page{};
                    const auto value = static_cast<std::byte>((seed + operation) & 0xFFU);
                    actual->data()[0] = value;
                    page[0] = value;
                    logical.push_back(page);
                    durable.push_back(DiskManager::Page{});
                    if ((random() % 3U) == 0) {
                        held.push_back({Guard(std::move(*actual)), *expected, nextPageId});
                    } else {
                        actual->drop();
                        model.release(*expected);
                    }
                }
            } else if (choice < 75U && !held.empty()) {
                const auto index = static_cast<std::size_t>(random() % held.size());
                dropGuard(held[index], model);
                held[index] = std::move(held.back());
                held.pop_back();
            } else if (choice < 85U) {
                const auto pageId = static_cast<PageId>(1U + random() % (logical.size() - 1U));
                require(pool.flushPage(pageId) == model.flushPage(pageId),
                        "random flushPage residency result disagreed with model");
            } else if (choice < 90U) {
                pool.flushAll();
                model.flushAll();
            } else if (!held.empty()) {
                const auto index = static_cast<std::size_t>(random() % held.size());
                if (auto* write = std::get_if<minidb::WritePageGuard>(&held[index].guard)) {
                    const auto offset = static_cast<std::size_t>(random() % DiskManager::PAGE_SIZE);
                    const auto value = static_cast<std::byte>((operation * 7U) & 0xFFU);
                    write->data()[offset] = value;
                    logical[held[index].pageId][offset] = value;
                    model.markDirty(held[index].modelFrame);
                }
            }

            if (operation % 97U == 0) {
                validateAgainstModel(pool, model, logical, seed, operation);
            }
        }

        for (auto& guard : held) dropGuard(guard, model);
        held.clear();
        pool.flushAll();
        model.flushAll();
        validateAgainstModel(pool, model, logical, seed, operationCount);
        for (PageId pageId = 1; pageId < logical.size(); ++pageId) {
            DiskManager::Page page{};
            disk.readPage(pageId, page);
            require(page == logical[pageId] && page == durable[pageId],
                    "random explicit flush did not synchronize model and disk");
        }
    }
    {
        DiskManager disk(database.path().string());
        require(disk.pageCount() == logical.size(), "random reopen page count changed");
        BufferPoolManager pool(disk, capacity, k);
        for (PageId pageId = 1; pageId < logical.size(); ++pageId) {
            auto page = pool.fetchPageRead(pageId);
            require(page.has_value()
                        && std::equal(page->data().begin(), page->data().end(), logical[pageId].begin()),
                    "random reopen content mismatch");
        }
        pool.validate();
    }
}

} // namespace

int main() {
    try {
        runScenario(7, 2, 1);
        runScenario(42, 3, 2);
        runScenario(2'026, 4, 3);
        runScenario(99'173, 5, 2);
        std::cout << "BufferPoolManager randomized model tests passed (24000 operations)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BufferPoolManager randomized test failure: " << error.what() << '\n';
        return 1;
    }
}
