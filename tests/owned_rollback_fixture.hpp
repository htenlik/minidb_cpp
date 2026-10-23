#pragma once

#include "minidb/checkpoint_control.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/owned_rollback_analysis.hpp"
#include "test_utils.hpp"

namespace minidb::test {
// The only issuer of the private opt-in. No production translation unit includes
// this header, and no constructor/SQL/CLI/environment activation path exists.
class OwnedRollbackTestAccess {
public:
    static TransactionId begin(RecoveryCoordinator& recovery, const DatabaseAccessGate& gate,
                               const DatabaseAccessGate::Lease& lease) {
        require(lease.owns(gate) && lease.mode()==AccessMode::ReadWrite,"Test owned BEGIN lacks exclusive lease");
        require(!recovery.hasActiveStatement(),"Test owned BEGIN overlaps another live context");
        const auto id=recovery.beginTransaction();recovery.beginMutation(id);
        std::lock_guard lock(recovery.contextsMutex_);
        recovery.ownedGate_=&gate;recovery.ownedLease_=&lease;
        recovery.active_->rollbackProtocol=RollbackProtocol::OwnedAppend;
        return id;
    }
    static RecoveryStats recover(RecoveryManager& recovery, const DatabaseAccessGate& gate,
                                 const DatabaseAccessGate::Lease& lease) {
        recovery.ownedGate_=&gate;recovery.ownedLease_=&lease;
        try {auto result=recovery.recover();recovery.ownedGate_=nullptr;recovery.ownedLease_=nullptr;return result;}
        catch (...) {recovery.ownedGate_=nullptr;recovery.ownedLease_=nullptr;throw;}
    }
};
struct OwnedFixture {
    DatabaseAccessGate gate;
    DatabaseAccessGate::Lease lease=gate.acquireExclusive();
    DiskManager disk;
    LogManager log;
    CheckpointControl control;
    RecoveryStats startup;
    RecoveryCoordinator recovery;
    BufferPoolManager pool;
    DatabaseMetadataManager metadata;
    PageAllocator allocator;
    OwnedFixture(const std::string& path, WalUpdateMode mode=WalUpdateMode::Adaptive,
                 WalStorageMode storage=WalStorageMode::Segmented,
                 RedoPolicy policy=RedoPolicy::PageLsnSelectiveRedo)
        : disk(path),log(walPathForDatabase(path),4096,LogOpenMode::DeferredRecovery,storage,9000),
          control(path+".ckpt"),startup(restart(disk,log,control,gate,lease,policy)),
          recovery(disk,log,startup.nextTransactionId,mode),pool(disk,64,2,&log,&recovery),
          metadata(disk,recovery,log),allocator(pool,disk,&metadata) {recovery.attachBufferPool(pool);}
    static RecoveryStats restart(DiskManager& disk,LogManager& log,CheckpointControl& control,
        const DatabaseAccessGate& gate,const DatabaseAccessGate::Lease& lease,RedoPolicy policy) {
        RecoveryManager manager(disk,log,&control,false,policy);
        return OwnedRollbackTestAccess::recover(manager,gate,lease);
    }
    void begin() {static_cast<void>(OwnedRollbackTestAccess::begin(recovery,gate,lease));}
    PageId allocate(std::byte marker=std::byte{0xA7}) {
        const auto page=allocator.allocatePage();
        {auto guard=requireWritePage(pool,page,"owned tuple bytes");guard.data()[100]=marker;}
        return page;
    }
};
} // namespace minidb::test
