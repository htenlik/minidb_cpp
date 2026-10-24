#include "owned_rollback_fixture.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/page_lsn.hpp"
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace minidb;
using test::require;
using test::OwnedFixture;
void waitCrash(pid_t child) {
    int status=0;require(child>0 && ::waitpid(child,&status,0)==child && WIFEXITED(status)
        && WEXITSTATUS(status)==86,"Owned crash point was not reached, status="+std::to_string(status));
}
void legacyAppendDeltaBase() {
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented})
       for(auto policy:{RedoPolicy::PageLsnSelectiveRedo,RedoPolicy::AlwaysRedo}) {
        test::TemporaryDatabase db("legacy_append_delta_base");
        {OwnedFixture f(db.path().string(),mode,storage,policy);f.recovery.beginStatement();
         require(f.allocate()==1 && f.allocate()==2,"Legacy setup allocation");f.recovery.commitStatement();
         f.recovery.beginStatement();f.allocator.releasePage(2);f.recovery.commitStatement();f.pool.flushAll();f.disk.sync();}
        OwnedFixture f(db.path().string(),mode,storage,policy);f.allocator.validate();
        require(f.allocator.freePageIds()==std::vector<PageId>{2},"Appended delta retained later Free header/PageLSN");
        DiskManager::Page physical{};f.disk.readPhysicalPage(1,physical);
        require(physical[100]==std::byte{0xA7},"Legacy committed page changed");
    }
}
void directed() {
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented}) {
        test::TemporaryDatabase db("owned_cleanup_directed");
        {OwnedFixture f(db.path().string(),mode,storage);
         f.begin();f.recovery.prepareForPhysicalPageAppend(f.disk.pageCount());
         f.recovery.rollbackStatement();require(f.disk.pageCount()==1 && f.allocator.freePageIds().empty(),"Absent reservation fabricated page");
         f.begin();{auto p=f.pool.newPageWrite();require(p.has_value(),"Uninitialized append");}
         f.recovery.rollbackStatement();require(f.disk.pageCount()==2 && f.allocator.freePageIds()==std::vector<PageId>{1},"Uninitialized append not reclaimed");
         f.begin();require(f.allocate()==1,"Preexisting free reuse");const auto second=f.allocate();const auto third=f.allocate();
         require(second==2 && third==3,"Mixed appended/reused IDs");
         f.allocator.releasePage(second);require(f.allocate()==second,"Same-owner release/reuse");
         f.allocator.releasePage(third); // unlinked by page-0 UNDO before cleanup
         f.metadata.updateCatalogRootPageId(third);f.pool.flushAll();
         f.recovery.rollbackStatement();
         require(f.disk.pageCount()==4 && f.allocator.freePageIds()==std::vector<PageId>({3,2,1})
            && f.disk.databaseHeader().catalogRootPageId==INVALID_PAGE_ID,"Mixed rollback lost metadata/free membership");
         require(f.recovery.lastRollbackStats().pagesTruncated==0,"Owned rollback truncated extent");
         for(unsigned n=0;n<100;++n) {f.begin();for(unsigned i=0;i<3;++i)static_cast<void>(f.allocate());
             f.recovery.rollbackStatement();f.allocator.validate();require(f.disk.pageCount()==4,"Equivalent rollback workload leaks pages");}
         // Keep unrelated committed resident bytes/identity across scoped discard.
         f.recovery.beginStatement();const auto unrelated=f.allocate(std::byte{0xBE});f.recovery.commitStatement();
         f.pool.flushAll();const auto before=*f.pool.residentPageCopy(unrelated);const auto lsn=f.pool.pageLsn(unrelated);
         f.begin();const auto owned=f.allocate();
         {auto pin=requireReadPage(f.pool,owned,"pinned owned page");
          test::requireThrows<std::logic_error>([&]{f.recovery.rollbackStatement();},"Pinned cleanup accepted");}
         f.recovery.rollbackStatement();
         require(f.pool.isResident(unrelated) && *f.pool.residentPageCopy(unrelated)==before
            && f.pool.pageLsn(unrelated)==lsn,"Owned invalidation changed unrelated resident page");
        }
        {OwnedFixture f(db.path().string(),mode,storage);f.allocator.validate();
         require(f.disk.pageCount()==4,"Restart changed high-water extent");}
    }
    std::cout<<"Owned directed modes/stores=6, steady-state rollback cycles=600\n";
}
void syntheticWinner() {
    for(auto policy:{RedoPolicy::PageLsnSelectiveRedo,RedoPolicy::AlwaysRedo}) {
        test::TemporaryDatabase db("owned_cleanup_winner");DiskManager::Page winner{};winner[100]=std::byte{0xBE};
        {DiskManager disk(db.path().string());while(disk.pageCount()<101)static_cast<void>(disk.appendPage());
         LogManager log(walPathForDatabase(db.path().string()));
         auto t1=log.append({LogRecordType::BeginOwned,1,INVALID_LSN,encodeOwnedBegin({101})});
         t1=log.append({LogRecordType::PageReserve,1,t1,encodePageReserveLogPayload({101,PageAllocationSource::Appended,101})});
         log.flushUpTo(t1);require(disk.appendPage()==101,"P101 append");
         DiskManager::Page loser{};loser[100]=std::byte{0xAA};
         t1=log.append({LogRecordType::PageUpdate,1,t1,encodePageUpdateLogPayload({101,false,{},loser})});
         auto t2=log.append({LogRecordType::BeginOwned,2,INVALID_LSN,encodeOwnedBegin({102})});
         t2=log.append({LogRecordType::PageReserve,2,t2,encodePageReserveLogPayload({102,PageAllocationSource::Appended,102})});
         log.flushUpTo(t2);require(disk.appendPage()==102,"P102 append");
         t2=log.append({LogRecordType::PageUpdate,2,t2,encodePageUpdateLogPayload({102,false,{},winner})});
         t2=log.append({LogRecordType::Commit,2,t2,{}});log.flushUpTo(t2);
         disk.writePhysicalPage(101,loser);disk.writePhysicalPage(102,winner);disk.sync();static_cast<void>(t1);}
        OwnedFixture f(db.path().string(),WalUpdateMode::FullPage,WalStorageMode::LegacySingleFile,policy);
        DiskManager::Page actual{};f.disk.readPhysicalPage(102,actual);
        require(actual==winner && f.disk.pageCount()==103 && f.allocator.freePageIds()==std::vector<PageId>{101},"Owned loser lost independently logged winner");
    }
    // Live variant: the independently committed higher page is still resident
    // and dirty. A historical suffix discard would lose it even without truncate.
    test::TemporaryDatabase db("owned_live_winner");OwnedFixture f(db.path().string());
    while(f.disk.pageCount()<101)static_cast<void>(f.disk.appendPage());
    f.begin();const auto loser=f.allocate(std::byte{0xAA});const auto t1=f.recovery.activeTransactionId();
    f.recovery.prepareTransaction(t1);
    const auto t2=f.recovery.beginTransaction();f.recovery.beginMutation(t2);
    const auto winner=f.allocate(std::byte{0xBE});f.recovery.commitTransaction(t2);
    const auto before=*f.pool.residentPageCopy(winner);const auto lsn=f.pool.pageLsn(winner);
    f.recovery.rollbackTransaction(t1);
    require(loser==101 && winner==102 && f.disk.pageCount()==103 && f.pool.isResident(winner)
        && *f.pool.residentPageCopy(winner)==before && f.pool.pageLsn(winner)==lsn
        && f.allocator.freePageIds()==std::vector<PageId>{101},"Live owned rollback discarded higher committed frame");
}
void interruptedRecoveryAndRetention() {
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented}) {
        test::TemporaryDatabase db("owned_repeated_restart");DiskManager::Page original{};
        {OwnedFixture f(db.path().string(),mode,storage);f.recovery.beginStatement();
         require(f.allocate()==1 && f.allocate()==2,"Setup IDs");f.recovery.commitStatement();
         f.recovery.beginStatement();f.allocator.releasePage(2);f.recovery.commitStatement();f.pool.flushAll();f.disk.sync();
         f.disk.readPhysicalPage(1,original);}
        auto child=::fork();
        if(child==0) {std::string phase="open";try {OwnedFixture f(db.path().string(),mode,storage);phase="begin";f.begin();
            {auto p=requireWritePage(f.pool,1,"existing bytes");p.data()[100]=std::byte{0xFE};}
            phase="reuse";require(f.allocate()==2,"Reuse before append");phase="append";for(unsigned i=0;i<4;++i)static_cast<void>(f.allocate());
            f.metadata.updateCatalogRootPageId(3);f.recovery.prepareStatement();
            CheckpointManager checkpoints(f.recovery,f.pool,f.disk,f.log,f.control,f.startup);
            checkpoints.attachAccessGate(f.gate);
            static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy,f.lease));
            require(f.log.containsLsn(f.recovery.transactionSnapshot().front().beginLsn),"Live owned ATT lost BEGIN");
            phase="flush/rollback";f.pool.flushAll();::setenv("MINIDB_FAILPOINT","recovery_after_compensation_page_write",1);
            f.recovery.rollbackStatement();::_exit(99);
        }catch(const std::exception& e){std::cerr<<"initial "<<phase<<" mode="<<static_cast<int>(mode)<<" storage="<<static_cast<int>(storage)<<": "<<e.what()<<'\n';::_exit(98);}}
        waitCrash(child);
        for(const auto* point:{"owned_after_reclaim_page_3","owned_after_reclaim_page_5"}) {
            child=::fork();if(child==0){try {::setenv("MINIDB_FAILPOINT",point,1);
                OwnedFixture f(db.path().string(),mode,storage);::_exit(99);
            }catch(const std::exception& e){std::cerr<<point<<" mode="<<static_cast<int>(mode)<<" storage="<<static_cast<int>(storage)<<": "<<e.what()<<'\n';::_exit(98);}}waitCrash(child);
        }
        for(unsigned n=0;n<2;++n){OwnedFixture f(db.path().string(),mode,storage);DiskManager::Page actual{};
            f.disk.readPhysicalPage(1,actual);
            require(actual==original && f.disk.pageCount()==7 && f.allocator.freePageIds()==std::vector<PageId>({6,5,4,3,2})
                && f.disk.databaseHeader().catalogRootPageId==INVALID_PAGE_ID,"Repeated startup cleanup lost logical/free state");}
    }
    for(const auto* point:{"owned_after_intent_force","owned_after_progress_force"}) {
        test::TemporaryDatabase db("owned_checkpoint_blocked");
        {OwnedFixture f(db.path().string());f.begin();static_cast<void>(f.allocate());
         const auto begin=f.recovery.transactionSnapshot().front().beginLsn;
         ::setenv("MINIDB_THROWPOINT",point,1);
         test::requireThrows<std::runtime_error>([&]{f.recovery.rollbackStatement();},"Cleanup interruption missing");
         ::unsetenv("MINIDB_THROWPOINT");
         CheckpointManager checkpoints(f.recovery,f.pool,f.disk,f.log,f.control,f.startup);
         checkpoints.attachAccessGate(f.gate);const auto end=f.log.lastValidOffset();
         test::requireThrows<std::logic_error>([&]{static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy,f.lease));},"Checkpoint crossed incomplete cleanup");
         require(!f.recovery.checkpointSafe() && f.log.containsLsn(begin) && f.log.lastValidOffset()==end,"Incomplete cleanup lost WAL");}
        OwnedFixture f(db.path().string());require(f.allocator.freePageIds()==std::vector<PageId>{1},"Blocked checkpoint cleanup did not resume");
    }
}
void crashes() {
    const char* points[]={"append_after_reservation_force","append_after_physical_extension",
        "owned_before_cleanup_begin","owned_after_cleanup_begin_append","owned_after_cleanup_begin_force",
        "owned_before_intent_append","owned_after_intent_append","owned_after_intent_force","owned_before_reclaim_append",
        "reclaim_after_append","reclaim_after_wal_force","reclaim_after_free_write","reclaim_after_metadata_write",
        "reclaim_before_database_sync","reclaim_after_database_sync","owned_before_progress_append",
        "owned_after_progress_append","owned_after_progress_force","owned_before_abort_append","owned_after_abort_append","owned_after_abort_force"};
    unsigned count=0;
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented})
       for(auto policy:{RedoPolicy::PageLsnSelectiveRedo,RedoPolicy::AlwaysRedo})
        for(bool metadataFirst:{false,true}) for(const auto* point:points) {
            test::TemporaryDatabase db("owned_cleanup_crash");{OwnedFixture f(db.path().string(),mode,storage);}
            const auto child=::fork();
            if(child==0) {try {OwnedFixture f(db.path().string(),mode,storage,policy);
                if(metadataFirst)::setenv("MINIDB_RECLAIM_PAGE0_FIRST","1",1);
                ::setenv("MINIDB_FAILPOINT",point,1);f.begin();static_cast<void>(f.allocate());
                f.recovery.rollbackStatement();::_exit(99);
            }catch(const std::exception& e){std::cerr<<point<<": "<<e.what()<<'\n';::_exit(98);}}
            waitCrash(child);
            try {for(unsigned restart=0;restart<2;++restart) {OwnedFixture f(db.path().string(),mode,storage,policy);
                require(f.disk.pageCount()==1 ? f.allocator.freePageIds().empty()
                    : f.disk.pageCount()==2 && f.allocator.freePageIds()==std::vector<PageId>{1},"Crash left orphan/duplicate");
                f.allocator.validate();}++count;
            }catch(const std::exception& e){throw std::runtime_error(std::string(point)+": "+e.what());}
        }
    std::cout<<"Owned hard-crash prefixes="<<count<<" (all modes/stores/REDO policies/write orders)\n";
    unsigned tailCases=0;
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented})
       for(const auto* point:{"owned_tail_before_reclaim_append","owned_tail_after_reclaim_append","owned_tail_after_reclaim_force"}) {
        test::TemporaryDatabase db("owned_tail_crash");{OwnedFixture f(db.path().string(),mode,storage);}
        auto child=::fork();if(child==0) {try {OwnedFixture f(db.path().string(),mode,storage);
            ::setenv("MINIDB_FAILPOINT","owned_after_intent_force",1);f.begin();static_cast<void>(f.allocate());
            f.recovery.rollbackStatement();::_exit(99);
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';::_exit(98);}}waitCrash(child);
        child=::fork();if(child==0){try{::setenv("MINIDB_FAILPOINT",point,1);
            OwnedFixture f(db.path().string(),mode,storage);::_exit(99);
        }catch(const std::exception& e){std::cerr<<point<<": "<<e.what()<<'\n';::_exit(98);}}waitCrash(child);
        for(unsigned n=0;n<2;++n){OwnedFixture f(db.path().string(),mode,storage);
            require(f.disk.pageCount()==2 && f.allocator.freePageIds()==std::vector<PageId>{1},"Interrupted intent-tail completion failed");
            const auto scan=f.log.scan();require(std::count_if(scan.records.begin(),scan.records.end(),[](const auto& r){
                return r.type==LogRecordType::PageReclaim;})==1,"Interrupted tail produced duplicate reclaim");}
        ++tailCases;
       }
    std::cout<<"Owned interrupted-tail cases="<<tailCases<<" (two hard exits and two clean restarts each)\n";
}
void negative() {
    for(unsigned scenario=0;scenario<5;++scenario) {
        test::TemporaryDatabase db("owned_cleanup_unsafe");OwnedFixture f(db.path().string());f.begin();
        const auto p=f.allocate();f.recovery.prepareStatement();
        if(scenario==0) {DiskManager::Page forged{};forged[200]=std::byte{1};f.disk.writePhysicalPage(p,forged);}
        if(scenario==1) f.lease.reset();
        if(scenario==2) { // an unlogged catalog reference must not be discarded
            f.disk.updateCatalogRootPageId(p);
        }
        if(scenario==3) { // unlogged duplicate membership, not a recovery skip
            f.disk.writePhysicalPage(p,reclaimedPage({p,INVALID_PAGE_ID,f.disk.pageCount()},INVALID_LSN));
            f.disk.updateFreeListRootPageId(p);
        }
        if(scenario==4) static_cast<void>(f.disk.appendPage()); // unreserved extent cannot be silently accepted
        bool rejected=false;try{f.recovery.rollbackStatement();}catch(const std::exception&){rejected=true;}
        require(rejected,"Unsafe ownership accepted");
    }
    // Real scanned histories, with no physical effects accepted for malformed
    // chains, cleanup claims, terminal states or unretained BEGIN ancestry.
    for(unsigned scenario=0;scenario<12;++scenario) {
        test::TemporaryDatabase db("owned_cleanup_bad_wal");DatabaseAccessGate gate;auto lease=gate.acquireExclusive();
        DiskManager disk(db.path().string());static_cast<void>(disk.appendPage());
        LogManager log(walPathForDatabase(db.path().string()));
        const auto begin=log.append({scenario==0?LogRecordType::Begin:LogRecordType::BeginOwned,1,INVALID_LSN,
            scenario==0?encodeBeginLogPayload({1}):encodeOwnedBegin({1})});
        const auto reserve=log.append({LogRecordType::PageReserve,1,begin,encodePageReserveLogPayload({1,PageAllocationSource::Appended,1})});
        auto last=reserve;
        if(scenario==1) {
            const auto update=log.append({LogRecordType::PageUpdate,1,last,encodePageUpdateLogPayload({0,true,{},{}})});
            last=log.append({LogRecordType::Compensation,1,update,encodeCompensationLogPayload({0,true,false,begin,update,{}})});
        }
        if(scenario==2) last=log.append({LogRecordType::Abort,1,last,{}});
        else {
            last=log.append({LogRecordType::RollbackCleanupBegin,1,last,encodeCleanupBegin({begin,1})});
            if(scenario==3) last=log.append({LogRecordType::RollbackCleanupProgress,1,last,encodeCleanupProgress({reserve,INVALID_LSN,1,1})});
            else if(scenario==4) last=log.append({LogRecordType::RollbackCleanupProgress,1,last,encodeCleanupProgress({reserve,last,1,2})});
            else if(scenario==5) last=log.append({LogRecordType::RollbackCleanupIntent,2,last,
                encodeCleanupIntent({reserve,cleanupReclaimLsn(log.lastValidOffset()),{1,INVALID_PAGE_ID,2}})});
            else if(scenario==6) last=log.append({LogRecordType::Commit,1,last,{}});
            else if(scenario==7) last=log.append({LogRecordType::RollbackCleanupBegin,1,last,encodeCleanupBegin({begin,1})});
            else if(scenario==8) last=log.append({LogRecordType::RollbackCleanupProgress,1,last,encodeCleanupProgress({reserve,INVALID_LSN,2,1})});
            else if(scenario==9) {
                auto other=log.append({LogRecordType::BeginOwned,2,INVALID_LSN,encodeOwnedBegin({2})});
                last=log.append({LogRecordType::PageUpdate,2,other,encodePageUpdateLogPayload({1,true,{},{}})});
            } else if(scenario==10) last=log.append({LogRecordType::PageReclaim,0,INVALID_LSN,encodePageReclaim({1,INVALID_PAGE_ID,2})});
            else if(scenario==11) {
                DiskManager::Page foreign{};foreign[200]=std::byte{0xFA};disk.writePhysicalPage(1,foreign);
                const PageReclaimPayload forged{1,INVALID_PAGE_ID,2,INVALID_LSN,INVALID_LSN,crc32c(foreign)};
                last=log.append({LogRecordType::RollbackCleanupIntent,1,last,
                    encodeCleanupIntent({reserve,cleanupReclaimLsn(log.lastValidOffset()),forged})});
                last=log.append({LogRecordType::PageReclaim,0,INVALID_LSN,encodePageReclaim(forged)});
            }
            if(scenario==3) last=log.append({LogRecordType::Abort,1,last,{}});
        }
        log.flushUpTo(last);DiskManager::Page before{};disk.readPhysicalPage(0,before);
        RecoveryManager recovery(disk,log);
        test::requireThrows<WalError>([&]{static_cast<void>(test::OwnedRollbackTestAccess::recover(recovery,gate,lease));},"Bad owned history accepted");
        DiskManager::Page after{};disk.readPhysicalPage(0,after);require(after==before && disk.pageCount()==2,"Malformed owned WAL changed metadata/extent");
    }
    {test::TemporaryDatabase db("owned_bad_checkpoint_att");DatabaseAccessGate gate;auto lease=gate.acquireExclusive();
     DiskManager disk(db.path().string());LogManager log(walPathForDatabase(db.path().string()));
     const auto begin=log.append({LogRecordType::BeginOwned,1,INVALID_LSN,encodeOwnedBegin({1})});
     const auto reserve=log.append({LogRecordType::PageReserve,1,begin,encodePageReserveLogPayload({1,PageAllocationSource::Appended,1})});
     log.flushUpTo(reserve);static_cast<void>(disk.appendPage());disk.sync();
     const auto cpBegin=log.append({LogRecordType::FuzzyCheckpointBegin,0,INVALID_LSN,encodeFuzzyCheckpointBeginLogPayload({1,INVALID_LSN})});
     const auto cpEnd=log.append({LogRecordType::FuzzyCheckpointEnd,0,INVALID_LSN,encodeFuzzyCheckpointEndLogPayload({1,cpBegin,2,2,{},{}})});
     log.flushUpTo(cpEnd);CheckpointControl control(db.path().string()+".ckpt");
     control.publish({1,1,cpEnd,cpBegin,2,2,log.lastValidOffset()});
     RecoveryManager recovery(disk,log,&control);
     test::requireThrows<WalError>([&]{static_cast<void>(test::OwnedRollbackTestAccess::recover(recovery,gate,lease));},"Omitted owned ATT accepted");}
}
}
int main(int argc,char** argv) {
    try {if(argc==2 && std::string_view(argv[1])=="--crash")crashes();
        else if(argc==2 && std::string_view(argv[1])=="--restart")interruptedRecoveryAndRetention();
        else {legacyAppendDeltaBase();directed();syntheticWinner();interruptedRecoveryAndRetention();negative();}
        std::cout<<"Owned cleanup tests passed; production activation remains disabled\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
