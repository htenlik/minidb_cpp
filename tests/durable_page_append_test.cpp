#include "minidb/checkpoint_manager.hpp"
#include "minidb/checkpoint_log.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <thread>

namespace {
using namespace minidb;
using test::require;

class AppendWitness final : public PageRecoveryHook {
public:
    AppendWitness(DiskManager& disk, LogManager& log, RecoveryCoordinator& recovery)
        : disk_(disk), log_(log), recovery_(recovery) {}
    BufferPoolManager* pool = nullptr;
    std::size_t preparations = 0;
    PageId lastPreAppendCount = 0;
    void prepareForPhysicalPageAppend() override {
        // A different thread must be able to take buffer metadata while the
        // recovery hook is running. CTest's timeout catches a lock regression.
        std::thread observer([&] { static_cast<void>(pool->stats()); });
        observer.join();
        lastPreAppendCount = disk_.pageCount();
        recovery_.prepareForPhysicalPageAppend();
        const auto entry = recovery_.transactionSnapshot().front();
        require(isValidLsn(entry.beginLsn) && log_.durableLsn() >= entry.beginLsn,
                "BEGIN was not durable in pre-append hook");
        const auto physical = readWalRecordAt(log_.path(), entry.beginLsn);
        require(physical.type == LogRecordType::Begin
                && physical.transactionId == entry.transactionId
                && decodeBeginLogPayload(physical.payload).startPageCount == entry.startPageCount,
                "Physical WAL does not contain the owning BEGIN/boundary");
        require(disk_.pageCount() == lastPreAppendCount, "Hook ran after physical extension");
        ++preparations;
    }
    void notePageWriteIntent(PageId page, const DiskManager::Page& before) override {
        require(disk_.pageCount() == lastPreAppendCount + 1,
                "Frame installation did not follow append preparation");
        recovery_.notePageWriteIntent(page, before);
    }
    Lsn preparePageForWrite(PageId page, DiskManager::Page& after) override {
        return recovery_.preparePageForWrite(page, after);
    }
private:
    DiskManager& disk_;
    LogManager& log_;
    RecoveryCoordinator& recovery_;
};

void orderingAndOneForce() {
    test::TemporaryDatabase db("durable_append_order");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryCoordinator recovery(disk, log);
    AppendWitness witness(disk, log, recovery);
    BufferPoolManager pool(disk, 4, 2, &log, &witness);
    witness.pool = &pool;
    recovery.attachBufferPool(pool);
    const auto id = recovery.beginTransaction();
    recovery.beginMutation(id);
    log.resetStats();
    Lsn begin = INVALID_LSN;
    for (int i = 0; i < 3; ++i) {
        auto page = pool.newPageWrite();
        require(page.has_value(), "Could not append page");
        require(!recovery.checkpointSafe(), "Half-completed append was checkpoint-safe");
        page->data()[80] = static_cast<std::byte>(i + 1);
        page->drop();
        const auto current = recovery.transactionSnapshot().front();
        if (i == 0) begin = current.beginLsn;
        require(current.beginLsn == begin && current.startPageCount == 1,
                "Repeated append changed transaction boundary/BEGIN");
        require(log.stats().fsyncCalls == 1, "Repeated append unnecessarily forced WAL");
        recovery.prepareTransaction(id);
    }
    const auto last = recovery.lastLsn(id);
    recovery.commitTransaction(id);
    const auto records = log.scan().records;
    require(std::count_if(records.begin(), records.end(), [](const auto& record) {
                return record.type == LogRecordType::Begin;
            }) == 1 && records.back().prevLsn == last,
            "Append reset prevLSN or duplicated BEGIN");
    require(witness.preparations == 3 && log.stats().fsyncCalls == 2,
            "Unexpected BEGIN/COMMIT force count");
}

void bufferedBeginMustBeForced() {
    test::TemporaryDatabase db("durable_append_buffered_begin");
    DiskManager disk(db.path().string());
    const auto existing = disk.appendPage();
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryCoordinator recovery(disk, log);
    BufferPoolManager pool(disk, 3, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    recovery.beginStatement();
    { auto page = pool.fetchPageWrite(existing); page->data()[80] = std::byte{1}; }
    recovery.prepareStatement();
    const auto before = recovery.transactionSnapshot().front();
    require(isValidLsn(before.beginLsn) && !isValidLsn(log.durableLsn()),
            "Setup did not create a buffered BEGIN");
    log.resetStats();
    { auto page = pool.newPageWrite(); require(page.has_value(), "Append failed"); }
    const auto after = recovery.transactionSnapshot().front();
    require(after.beginLsn == before.beginLsn && after.lastLsn == before.lastLsn
            && log.durableLsn() >= before.beginLsn && log.stats().fsyncCalls == 1,
            "Buffered BEGIN was duplicated/not forced or prevLSN changed");
    recovery.rollbackStatement();
    require(disk.pageCount() == 2, "Rollback lost original boundary");
}

void rejectInvalidOwnershipAndFailures() {
    test::TemporaryDatabase db("durable_append_ownership");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryCoordinator recovery(disk, log);
    BufferPoolManager pool(disk, 2, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    test::requireThrows<std::logic_error>([&] { static_cast<void>(pool.newPageWrite()); },
                                        "Append without owning transaction succeeded");
    const auto first = recovery.beginTransaction(), second = recovery.beginTransaction();
    recovery.bindTransaction(first);
    for (auto id : {INVALID_TRANSACTION_ID, second, TransactionId{9999}}) {
        test::requireThrows<std::logic_error>([&] { recovery.prepareForPhysicalPageAppend(id); },
                                            "Append accepted invalid/unbound owner");
    }
    ::setenv("MINIDB_THROWPOINT", "append_after_begin_before_force", 1);
    test::requireThrows<std::runtime_error>([&] { static_cast<void>(pool.newPageWrite()); },
                                          "Pre-force failure was ignored");
    ::unsetenv("MINIDB_THROWPOINT");
    require(disk.pageCount() == 1 && pool.residentPageCount() == 0,
            "Pre-force failure extended/installed a page");
    recovery.rollbackTransaction(first);
    recovery.commitTransaction(second);
    recovery.beginStatement();
    ::setenv("MINIDB_THROWPOINT", "recovery_before_abort_append", 1);
    { auto page = pool.newPageWrite(); page->data()[80] = std::byte{2}; }
    test::requireThrows<std::runtime_error>([&] { recovery.rollbackStatement(); }, "Rollback injection failed");
    ::unsetenv("MINIDB_THROWPOINT");
    test::requireThrows<std::logic_error>([&] { static_cast<void>(pool.newPageWrite()); },
                                        "Append during ABORTING succeeded");
}

void allocatorReuseAndFuzzyAtt() {
    test::TemporaryDatabase db("durable_append_allocator_att");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryCoordinator recovery(disk, log);
    BufferPoolManager pool(disk, 8, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    DatabaseMetadataManager metadata(disk, recovery, log);
    PageAllocator allocator(pool, disk, &metadata);
    recovery.beginStatement();
    const auto free = allocator.allocatePage();
    allocator.releasePage(free);
    recovery.commitStatement();
    pool.flushAll();
    recovery.beginStatement();
    log.resetStats();
    require(allocator.allocatePage() == free && log.stats().fsyncCalls <= 1,
            "Free-list reuse bypassed its existing metadata WAL behavior");
    recovery.rollbackStatement();
    allocator.validate();
    require(allocator.freePageIds() == std::vector<PageId>{free}, "Reuse rollback did not restore free list");

    recovery.beginStatement();
    // Consume the free page, then genuinely extend the file.
    static_cast<void>(allocator.allocatePage());
    static_cast<void>(allocator.allocatePage());
    const auto entry = recovery.transactionSnapshot().front();
    CheckpointControl control(db.path().string() + ".ckpt");
    CheckpointManager checkpoints(recovery, pool, disk, log, control, {}, {0, 0, CheckpointMode::Fuzzy});
    DatabaseAccessGate gate;
    checkpoints.attachAccessGate(gate);
    auto lease = gate.acquireExclusive();
    test::requireThrows<std::logic_error>([&] {
        static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
    }, "Checkpoint captured an unfinished append");
    recovery.prepareStatement();
    static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
    const auto snapshot = recovery.checkpointTransactions();
    require(snapshot.size() == 1 && snapshot.front().beginLsn == entry.beginLsn
            && snapshot.front().startPageCount == entry.startPageCount,
            "Allocation's durable BEGIN was absent from live ATT");
    const auto selected = control.select(log);
    require(selected.slot.has_value(), "Fuzzy checkpoint was not published");
    const auto persisted = decodeFuzzyCheckpointEndLogPayload(
        log.readRecordAt(selected.slot->checkpointEndLsn).payload);
    require(persisted.activeTransactions == snapshot, "Persisted fuzzy ATT differs from live allocation context");
    recovery.rollbackStatement();
    allocator.validate();
}
} // namespace

int main() {
    try {
        orderingAndOneForce(); bufferedBeginMustBeForced();
        rejectInvalidOwnershipAndFailures(); allocatorReuseAndFuzzyAtt();
        std::cout << "durable_page_append_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        ::unsetenv("MINIDB_THROWPOINT");
        std::cerr << "durable_page_append_test failed: " << error.what() << '\n';
        return 1;
    }
}
