#include "minidb/wal_types.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <vector>

namespace {
using minidb::test::require;

struct Update {
    minidb::Lsn lsn;
    minidb::PageId pageId;
    std::uint64_t value;
};

void runRecoveryModel(std::uint64_t seed) {
    constexpr std::size_t OPERATIONS = 3'000;
    std::mt19937_64 random(seed);
    std::map<minidb::PageId, std::uint64_t> volatilePages;
    std::map<minidb::PageId, std::uint64_t> diskPages;
    std::map<minidb::PageId, minidb::Lsn> diskPageLsns;
    std::map<minidb::PageId, minidb::Lsn> dpt;
    std::vector<Update> wal;
    minidb::Lsn nextLsn = 64;

    for (std::size_t operation = 0; operation < OPERATIONS; ++operation) {
        const auto pageId = static_cast<minidb::PageId>(1 + random() % 96);
        const auto choice = random() % 10;
        if (choice < 7) {
            nextLsn += 48;
            const auto value = random();
            volatilePages[pageId] = value;
            dpt.try_emplace(pageId, nextLsn);
            wal.push_back({nextLsn, pageId, value});
        } else if (choice < 9) {
            const auto found = volatilePages.find(pageId);
            if (found != volatilePages.end()) {
                diskPages[pageId] = found->second;
                const auto latest = std::find_if(
                    wal.rbegin(), wal.rend(),
                    [pageId](const Update& update) { return update.pageId == pageId; });
                if (latest != wal.rend()) diskPageLsns[pageId] = latest->lsn;
                dpt.erase(pageId);
            }
        } else {
            const auto checkpointDpt = dpt;
            auto recoveredPages = diskPages;
            auto recoveredLsns = diskPageLsns;
            for (const auto& update : wal) {
                const auto found = checkpointDpt.find(update.pageId);
                if (found == checkpointDpt.end() || update.lsn < found->second) continue;
                const auto persisted = recoveredLsns.find(update.pageId);
                if (persisted != recoveredLsns.end() && persisted->second >= update.lsn) continue;
                recoveredPages[update.pageId] = update.value;
                recoveredLsns[update.pageId] = update.lsn;
            }
            for (const auto& [dirtyPage, recLsn] : checkpointDpt) {
                static_cast<void>(recLsn);
                require(recoveredPages[dirtyPage] == volatilePages[dirtyPage],
                        "Fuzzy recovery model lost a dirty-page update at seed "
                            + std::to_string(seed) + " operation "
                            + std::to_string(operation));
            }
        }
    }
}
} // namespace

int main() {
    try {
        for (const auto seed : {0xF0221ULL, 0xC0FFEEULL, 0x5E1EC7ULL, 0xA11CEULL}) {
            runRecoveryModel(seed);
        }
        std::cout << "fuzzy_recovery_model_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fuzzy_recovery_model_test failed: " << error.what() << '\n';
        return 1;
    }
}
