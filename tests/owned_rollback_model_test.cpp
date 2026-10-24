#include "owned_rollback_fixture.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/page_lsn.hpp"

#include <iostream>
#include <memory>
#include <random>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace minidb;
using test::require;
struct Storage : test::OwnedFixture {
    CheckpointManager checkpoints;
    Storage(const std::string& path, WalUpdateMode mode)
        : OwnedFixture(path, mode), checkpoints(recovery, pool, disk, log, control, startup) {
        checkpoints.attachAccessGate(gate);
    }
};
enum class Kind { Release, Allocate, Update };
struct Action { Kind kind; PageId page; std::uint32_t tag; };
using Pages = std::map<PageId, DiskManager::Page>;

// Preserve the prototype's ordered allocation/release/update actions, but use
// the current private authority and real exclusive checkpoint lease, not its
// old public constructor switch or an empty lease.
void apply(Storage& f, const std::vector<Action>& actions, bool fuzzy) {
    f.begin();
    for (const auto& a : actions) {
        if (a.kind == Kind::Release) f.allocator.releasePage(a.page);
        else {
            if (a.kind == Kind::Allocate)
                require(f.allocator.allocatePage() == a.page, "Allocation identity differs from model");
            auto p = requireWritePage(f.pool, a.page, "model update");
            byte_codec::writeUint32(p.data(), 128, a.tag);
        }
    }
    f.recovery.prepareStatement();
    if (fuzzy) {
        const auto tx = f.recovery.transactionSnapshot().front();
        static_cast<void>(f.checkpoints.checkpoint(CheckpointMode::Fuzzy, f.lease));
        require(f.log.containsLsn(tx.beginLsn)
            && f.checkpoints.stats().retentionFloorLsn <= tx.beginLsn, "Live ATT lost owned history");
    }
}
void compare(Storage& f, const Pages& pages, const std::vector<PageId>& free, PageId extent) {
    f.pool.flushAll();
    f.allocator.validate();
    f.pool.validate();
    require(f.disk.pageCount() == extent && f.allocator.freePageIds() == free,
        "Extent/free-list model mismatch");
    require(pages.size() + free.size() + 1 == extent, "Lost page or duplicate membership in model");
    for (const auto& [id, bytes] : pages) {
        DiskManager::Page physical{};
        f.disk.readPhysicalPage(id, physical);
        require(physical == bytes, "Committed page bytes changed");
        require(std::find(free.begin(), free.end(), id) == free.end(), "Committed page is free");
    }
    for (const auto id : free) {
        DiskManager::Page physical{};
        f.disk.readPhysicalPage(id, physical);
        require(isValidLsn(readPersistentPageLsn(physical)), "Free Page lost persistent PageLSN");
    }
    require(f.disk.databaseHeader().catalogRootPageId == INVALID_PAGE_ID, "Allocator changed catalog root");
}
void waitCrash(pid_t child) {
    int status = 0;
    require(child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status)
        && WEXITSTATUS(status) == 86, "Model hard crash not reached, status=" + std::to_string(status));
}
void run(std::uint64_t seed) {
    test::TemporaryDatabase db("owned_stage3_model");
    const auto mode = static_cast<WalUpdateMode>(seed % 3);
    auto storage = std::make_unique<Storage>(db.path().string(), mode);
    std::mt19937_64 rng(seed);
    Pages committed;
    std::vector<PageId> free;
    PageId extent = 1;
    std::uint64_t events = 0, cycle = 0, crashes = 0, reopens = 0, checkpoints = 0, deleted = 0;
    std::uint64_t recoveryInterruptions = 0;
    std::string phase = "plan";
    while (events < 7500) {
        const auto beforeFree = free;
        auto working = committed;
        std::vector<Action> actions;
        std::vector<PageId> appended;
        std::uint64_t pendingEvents = 0;
        if (!working.empty() && (rng() % 2 == 0 || working.size() > 16)) {
            auto it = working.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(rng() % working.size()));
            actions.push_back({Kind::Release, it->first, 0});
            free.insert(free.begin(), it->first);
            working.erase(it);
            ++pendingEvents;
        }
        if (!working.empty()) {
            const auto page = working.begin()->first;
            const auto tag = static_cast<std::uint32_t>(rng());
            actions.push_back({Kind::Update, page, tag});
            byte_codec::writeUint32(working.at(page), 128, tag);
            ++pendingEvents;
        }
        const auto allocations = 1 + rng() % 6;
        for (std::uint64_t i = 0; i < allocations; ++i) {
            PageId page;
            if (free.empty()) { page = extent++; appended.push_back(page); }
            else { page = free.front(); free.erase(free.begin()); }
            const auto tag = static_cast<std::uint32_t>(rng());
            actions.push_back({Kind::Allocate, page, tag});
            DiskManager::Page bytes{};
            byte_codec::writeUint32(bytes, 128, tag);
            working[page] = bytes;
            pendingEvents += 2; // allocate/reuse and payload update
            if (rng() % 3 == 0) {
                actions.push_back({Kind::Release, page, 0});
                actions.push_back({Kind::Allocate, page, tag});
                pendingEvents += 3; // release, reallocate, update
            }
        }
        const bool commit = rng() % 4 == 0;
        const bool crash = cycle % 7 == 0;
        const bool fuzzy = cycle % 11 == 0;
        try {
            phase = "apply";
            if (crash) {
                storage->pool.flushAll();
                deleted += storage->checkpoints.stats().segmentsReclaimed;
                storage.reset();
                const auto child = ::fork();
                if (child == 0) {
                    try {
                        Storage f(db.path().string(), mode);
                        apply(f, actions, fuzzy);
                        f.pool.flushAll(); // STEAL: require existing-page CLR UNDO
                        const char* points[] = {"owned_after_cleanup_begin_force",
                            "reclaim_after_wal_force", "owned_after_progress_force"};
                        ::setenv("MINIDB_FAILPOINT", commit ? "after_commit_sync"
                            : appended.empty() ? "owned_after_cleanup_begin_force" : points[cycle % 3], 1);
                        if (commit) f.recovery.commitStatement();
                        else f.recovery.rollbackStatement();
                        ::_exit(99);
                    } catch (const std::exception& e) {
                        std::cerr << "child seed=" << seed << " cycle=" << cycle << ": " << e.what() << '\n';
                        ::_exit(98);
                    }
                }
                waitCrash(child);
                phase = "restart";
                if (!commit && !appended.empty() && cycle % 3 == 0) {
                    for (const auto* point : {"owned_after_intent_force", "owned_tail_after_reclaim_force"}) {
                        const auto recovering = ::fork();
                        if (recovering == 0) {
                            try {::setenv("MINIDB_FAILPOINT", point, 1);
                                Storage f(db.path().string(), mode); ::_exit(99);
                            } catch (const std::exception& e) {
                                std::cerr << "recovery seed=" << seed << " cycle=" << cycle
                                    << " point=" << point << ": " << e.what() << '\n'; ::_exit(98);
                            }
                        }
                        waitCrash(recovering); ++recoveryInterruptions; pendingEvents += 2;
                    }
                }
                storage = std::make_unique<Storage>(db.path().string(), mode);
                ++crashes; ++reopens; pendingEvents += 2;
            } else {
                apply(*storage, actions, fuzzy);
                phase = commit ? "commit" : "rollback";
                if (commit) storage->recovery.commitStatement();
                else storage->recovery.rollbackStatement();
            }
            pendingEvents += 3; // BEGIN, prepare, terminal completion
            if (fuzzy) { ++checkpoints; ++pendingEvents; }
            if (commit) committed = working;
            else {
                free = beforeFree;
                for (const auto page : appended) free.insert(free.begin(), page);
            }
            phase = "compare";
            compare(*storage, committed, free, extent);
            if (cycle % 13 == 0) {
                phase = "checkpoint/reopen";
                static_cast<void>(storage->checkpoints.checkpoint(
                    cycle % 2 ? CheckpointMode::Sharp : CheckpointMode::Fuzzy, storage->lease));
                ++checkpoints; ++pendingEvents;
                deleted += storage->checkpoints.stats().segmentsReclaimed;
                storage.reset();
                storage = std::make_unique<Storage>(db.path().string(), mode);
                ++reopens; ++pendingEvents;
                compare(*storage, committed, free, extent);
            }
            // Do not credit planned actions or partially failed lifecycles.
            events += pendingEvents;
        } catch (const std::exception& e) {
            throw std::runtime_error("seed=" + std::to_string(seed) + " cycle=" + std::to_string(cycle)
                + " completed_events=" + std::to_string(events) + " phase=" + phase
                + " commit=" + std::to_string(commit) + " crash=" + std::to_string(crash)
                + " reopens=" + std::to_string(reopens) + ": " + e.what());
        }
        ++cycle;
    }
    deleted += storage->checkpoints.stats().segmentsReclaimed;
    require(deleted > 0, "Model never exercised actual WAL segment deletion");
    std::cout << "seed=" << seed << " completed_events=" << events << " transactions=" << cycle
        << " hard_crashes=" << crashes << " reopens=" << reopens << " checkpoints=" << checkpoints
        << " recovery_interruptions=" << recoveryInterruptions
        << " segments_deleted=" << deleted << " physical_pages=" << extent << '\n';
}
}
int main() {
    // Stop at the first failing seed; never silently continue to a passing subset.
    try {
        for (const auto seed : {0x12B3A4ULL, 0xC0FFEEULL, 0xDE1AULL, 0xA110CULL}) run(seed);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
