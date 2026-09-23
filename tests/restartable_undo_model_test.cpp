#include "minidb/wal_types.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {

using minidb::test::require;

struct Update {
    minidb::Lsn lsn;
    minidb::Lsn prevLsn;
    std::uint32_t before;
    std::uint32_t after;
};

struct Clr {
    minidb::Lsn lsn;
    minidb::Lsn compensatedUpdateLsn;
    minidb::Lsn undoNextLsn;
    std::uint32_t image;
};

void runModel(std::uint64_t seed, std::size_t& aggregateOperations) {
    std::mt19937_64 random(seed);
    constexpr minidb::Lsn BEGIN_LSN = 64;
    for (std::size_t history = 0; history < 100; ++history) {
        const auto updateCount = static_cast<std::size_t>(25 + (random() % 51));
        std::vector<Update> updates;
        updates.reserve(updateCount);
        auto previous = BEGIN_LSN;
        std::uint32_t value = static_cast<std::uint32_t>(random());
        const auto original = value;
        minidb::Lsn nextLsn = 128;
        for (std::size_t index = 0; index < updateCount; ++index) {
            const auto after = static_cast<std::uint32_t>(random());
            updates.push_back(Update{nextLsn, previous, value, after});
            previous = nextLsn;
            nextLsn += 128;
            value = after;
            ++aggregateOperations;
        }

        minidb::Lsn durableUndoNext = updates.back().lsn;
        minidb::Lsn pageLsn = updates.back().lsn;
        std::vector<Clr> durableClrs;
        std::size_t recoveryRestarts = 0;
        while (durableUndoNext != BEGIN_LSN) {
            const auto found = std::find_if(
                updates.begin(), updates.end(), [&](const Update& update) {
                    return update.lsn == durableUndoNext;
                });
            require(found != updates.end(), "Model undoNextLSN did not identify an update");
            const auto crashPhase = random() % 5;
            ++aggregateOperations;
            if (crashPhase <= 1) {
                // Before append or after append without force: no durable progress.
                ++recoveryRestarts;
                continue;
            }

            const Clr clr{nextLsn, found->lsn, found->prevLsn, found->before};
            nextLsn += 128;
            durableClrs.push_back(clr);
            durableUndoNext = clr.undoNextLsn;
            if (crashPhase == 2) {
                // WAL forced before page write. Restart REDO installs the image/CLR LSN.
                value = clr.image;
                pageLsn = clr.lsn;
                ++recoveryRestarts;
            } else {
                value = clr.image;
                pageLsn = clr.lsn;
                if (crashPhase == 3) ++recoveryRestarts;
            }
            require(value == found->before && pageLsn == clr.lsn,
                    "Durable CLR did not determine page contents and PageLSN");
        }
        ++aggregateOperations; // durable ABORT
        require(value == original, "Restartable model did not restore the original page");
        require(durableClrs.size() == updateCount,
                "Model compensated a user update more or less than once");
        require(!durableClrs.empty()
                    && durableClrs.back().undoNextLsn == BEGIN_LSN,
                "Final CLR did not terminate at BEGIN");
        static_cast<void>(recoveryRestarts);
    }
}

} // namespace

int main() {
    try {
        constexpr std::uint64_t SEEDS[]{
            0x11D50001ULL, 0x11D50002ULL, 0x11D50003ULL, 0x11D50004ULL,
        };
        std::size_t aggregateOperations = 0;
        for (const auto seed : SEEDS) runModel(seed, aggregateOperations);
        require(aggregateOperations >= 20'000,
                "Restartable UNDO model did not reach 20,000 aggregate operations");
        std::cout << "restartable_undo_model_test passed (" << aggregateOperations
                  << " operations; seeds 0x11D50001..0x11D50004)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "restartable_undo_model_test failed: " << error.what() << '\n';
        return 1;
    }
}
