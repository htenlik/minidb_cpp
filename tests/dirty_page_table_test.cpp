#include "minidb/buffer_pool_manager.hpp"
#include "minidb/page_recovery.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>

namespace {
using minidb::test::require;

class TestWal final : public minidb::WalFlushProvider {
public:
    void flushUpTo(minidb::Lsn target) override { durable_ = target; }
    [[nodiscard]] minidb::Lsn durableLsn() const noexcept override { return durable_; }
    [[nodiscard]] bool containsLsn(minidb::Lsn lsn) const noexcept override {
        return minidb::isValidLsn(lsn);
    }
private:
    minidb::Lsn durable_ = minidb::INVALID_LSN;
};

class SequencedRecoveryHook final : public minidb::PageRecoveryHook {
public:
    void notePageWriteIntent(minidb::PageId, const minidb::DiskManager::Page&) override {}
    minidb::Lsn preparePageForWrite(
        minidb::PageId, minidb::DiskManager::Page&) override {
        const auto result = next_;
        next_ += 100;
        return result;
    }
private:
    minidb::Lsn next_ = 100;
};

void mutate(minidb::BufferPoolManager& pool, minidb::PageId pageId, std::byte value) {
    auto guard = pool.fetchPageWrite(pageId);
    require(guard.has_value(), "Could not fetch DPT test page");
    (*guard).data()[100] = value;
    guard->drop();
    pool.prepareResidentPageForCommit(pageId);
}

void testExactDirtyPeriods() {
    minidb::test::TemporaryDatabase database("dpt_exact");
    minidb::DiskManager disk(database.path().string());
    TestWal wal;
    SequencedRecoveryHook recovery;
    minidb::BufferPoolManager pool(disk, 2, 2, &wal, &recovery);
    auto created = pool.newPageWrite();
    require(created.has_value(), "Could not allocate DPT test page");
    const auto pageId = created->pageId();
    created->data()[100] = std::byte{1};
    created->drop();
    pool.prepareResidentPageForCommit(pageId);
    require(pool.recLsn(pageId) == 100 && pool.pageLsn(pageId) == 100,
            "First clean-to-dirty update did not assign recLSN");

    mutate(pool, pageId, std::byte{2});
    require(pool.recLsn(pageId) == 100 && pool.pageLsn(pageId) == 200,
            "Repeated dirty-page write changed recLSN");
    require(pool.dirtyPageTableSnapshot()
                == std::vector<minidb::DirtyPageEntry>{{pageId, 100, 200}},
            "DPT snapshot does not expose the earliest dirty-period LSN");

    require(pool.flushPage(pageId), "Could not flush DPT test page");
    require(pool.isDirty(pageId) == false
                && pool.recLsn(pageId) == minidb::INVALID_LSN
                && pool.dirtyPageTableSnapshot().empty(),
            "Flush did not remove the page from the DPT");

    mutate(pool, pageId, std::byte{3});
    require(pool.recLsn(pageId) == pool.pageLsn(pageId)
                && pool.recLsn(pageId).value_or(0) > 200,
            "New dirty period did not receive a new recLSN");
    pool.validate();
}

void testDeterministicDptModel() {
    constexpr std::size_t OPERATIONS_PER_SEED = 6'000;
    constexpr std::uint64_t SEEDS[] = {0x11ULL, 0xD17ULL, 0xBADC0DEULL, 0x5EED1234ULL};
    for (const auto seed : SEEDS) {
        std::mt19937_64 random(seed);
        std::map<minidb::PageId, minidb::Lsn> dpt;
        std::map<minidb::PageId, minidb::Lsn> expected;
        minidb::Lsn lsn = 64;
        for (std::size_t operation = 0; operation < OPERATIONS_PER_SEED; ++operation) {
            const auto pageId = static_cast<minidb::PageId>(1 + random() % 128);
            lsn += 48;
            if ((random() % 5) == 0) {
                dpt.erase(pageId);
                expected.erase(pageId);
            } else {
                dpt.try_emplace(pageId, lsn);
                expected.try_emplace(pageId, lsn);
            }
            require(dpt == expected,
                    "DPT model diverged at seed " + std::to_string(seed)
                        + " operation " + std::to_string(operation));
            for (const auto& [page, recLsn] : dpt) {
                static_cast<void>(page);
                require(recLsn <= lsn, "DPT model recLSN exceeds current LSN");
            }
        }
    }
}
} // namespace

int main() {
    try {
        testExactDirtyPeriods();
        testDeterministicDptModel();
        std::cout << "dirty_page_table_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dirty_page_table_test failed: " << error.what() << '\n';
        return 1;
    }
}
