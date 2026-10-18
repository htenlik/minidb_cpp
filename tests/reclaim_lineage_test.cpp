#include "page_reclaim_fixture.hpp"
#include "minidb/reclaim_lineage.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/page_lsn.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace minidb;
using test::require;
void originalReproducer() {
    test::TemporaryDatabase db("reclaim_transitive_lineage");
    {test::ReclaimFixture f(db.path().string());f.initialize();
     static_cast<void>(f.reclaim(30));
     require(f.reuse(false)==30,"First reuse did not abort back to free");
     require(f.allocator.freePageIds()==std::vector<PageId>({30,20,10}),"First CLR did not restore membership");}
    const auto child=::fork();
    if(child==0) {
        test::ReclaimFixture f(db.path().string());f.recovery.beginStatement();
        require(f.allocator.allocatePage()==30,"Second reuse did not allocate P30");
        {auto p=requireWritePage(f.pool,30,"second opaque reuse");p.data()[100]=std::byte{0xBC};}
        f.pool.flushAll();f.disk.sync();::_exit(86);
    }
    int status=0;
    require(child>0 && ::waitpid(child,&status,0)==child && WIFEXITED(status)&&WEXITSTATUS(status)==86,"Hard crash not reached");
    static_cast<void>(test::recoverReclaim(db.path().string()));
    test::ReclaimFixture f(db.path().string());
    require(f.allocator.freePageIds()==std::vector<PageId>({30,20,10}),"Restart lost/doubled P30");
}
void initialize(test::ReclaimFixture& f,PageId page,unsigned kind,unsigned tag) {
    auto p=requireWritePage(f.pool,page,"lineage type transition");auto bytes=p.data();
    if(kind==2||kind==3)std::fill(bytes.begin(),bytes.end(),std::byte{0});
    if(kind==1) {
        SlottedPageView::initialize(bytes,page,f.disk.pageCount(),127);
        static_cast<void>(SlottedPageView(bytes,page,f.disk.pageCount()).insert(TupleBytes(16,static_cast<std::byte>(tag))));
    } else if(kind==2) {
        using namespace persistent_bplus_leaf_layout;
        std::copy(MAGIC.begin(),MAGIC.end(),bytes.begin());
        byte_codec::writeUint32(bytes,LAYOUT_VERSION_OFFSET,CURRENT_VERSION);
        byte_codec::writeUint32(bytes,HEADER_SIZE_OFFSET,HEADER_SIZE);
        byte_codec::writeUint32(bytes,NEXT_LEAF_PAGE_ID_OFFSET,INVALID_PAGE_ID);
        byte_codec::writeUint32(bytes,PREVIOUS_LEAF_PAGE_ID_OFFSET,INVALID_PAGE_ID);
    } else if(kind==3) {
        using namespace persistent_bplus_internal_layout;
        std::copy(MAGIC.begin(),MAGIC.end(),bytes.begin());
        byte_codec::writeUint32(bytes,LAYOUT_VERSION_OFFSET,CURRENT_VERSION);
        byte_codec::writeUint32(bytes,HEADER_SIZE_OFFSET,HEADER_SIZE);
        byte_codec::writeUint32(bytes,CHILD_COUNT_OFFSET,1);
        byte_codec::writeUint32(bytes,FIRST_CHILD_OFFSET,1);
    } else bytes[100]=static_cast<std::byte>(tag);
}
void verifyFree(test::ReclaimFixture& f,PageId page=30) {
    require(f.allocator.freePageIds()==std::vector<PageId>({page,20,10}),"Exact membership/head");
    DiskManager::Page bytes{};f.disk.readPhysicalPage(page,bytes);
    const auto lsn=readPersistentPageLsn(bytes);
    require(isValidLsn(lsn),"Free Page has no recovery operation LSN");
    const auto expected=reclaimedPage({page,20,f.disk.pageCount()},lsn);
    if(f.log.containsLsn(lsn)) {
        const auto record=f.log.readRecordAt(lsn);
        require(record.type==LogRecordType::PageReclaim||record.type==LogRecordType::Compensation,"Free LSN is not reclaim/CLR");
    } else require(lsn<f.log.oldestRetainedLsn(),"Missing required Free Page WAL boundary");
    require(bytes==expected,"Free bytes/CLR LSN/next link differ");
}
RecoveryStats recover(const std::string& path,WalStorageMode storage,RedoPolicy policy) {
    DiskManager disk(path);LogManager log(walPathForDatabase(path),4096,LogOpenMode::DeferredRecovery,storage,9000);
    CheckpointControl control(path+".ckpt");return RecoveryManager(disk,log,&control,false,policy).recover();
}
void waitCrash(pid_t child) {
    int status=0;require(child>0&&::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"Crash point not reached");
}
void matrix() {
    unsigned cases=0;
    std::map<std::tuple<unsigned,unsigned,unsigned,bool>,std::pair<DiskManager::Page,DiskManager::Page>> results;
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented})
       for(auto policy:{RedoPolicy::AlwaysRedo,RedoPolicy::PageLsnSelectiveRedo})
        for(unsigned kind=0;kind<4;++kind)for(bool commit:{false,true}) {
            test::TemporaryDatabase db("lineage_matrix");
            {test::ReclaimFixture f(db.path().string(),storage,mode);f.initialize();static_cast<void>(f.reclaim(30));
             for(unsigned n=0;n<3;++n){f.recovery.beginStatement();require(f.allocator.allocatePage()==30,"Repeated reuse identity");
                 initialize(f,30,n%4,n+1);f.recovery.prepareStatement();f.recovery.rollbackStatement();verifyFree(f);}}
            const auto child=::fork();
            if(child==0){test::ReclaimFixture f(db.path().string(),storage,mode);f.recovery.beginStatement();
                require(f.allocator.allocatePage()==30,"Final reuse identity");initialize(f,30,0,0xBC);
                // An intermediate opaque write is intentional, followed by a
                // typed initialization on the same transaction/base.
                f.recovery.prepareStatement();f.pool.flushAll();
                if(kind!=0){initialize(f,30,kind,0xBC);f.recovery.prepareStatement();f.pool.flushAll();}
                f.disk.sync();
                if(commit)f.recovery.commitStatement();
                ::_exit(86);}
            waitCrash(child);
            static_cast<void>(recover(db.path().string(),storage,policy));
            static_cast<void>(recover(db.path().string(),storage,policy));
            test::ReclaimFixture f(db.path().string(),storage,mode);
            if(!commit)verifyFree(f);
            else {require(f.allocator.freePageIds()==std::vector<PageId>({20,10}),"Committed reuse restored old head");
                auto p=requireReadPage(f.pool,30,"committed bytes");
                if(kind==0)require(p.data()[100]==std::byte{0xBC},"Committed opaque byte changed");
                else require(isValidLsn(readPersistentPageLsn(p.data())),"Typed state lost LSN");}
            DiskManager::Page page{},metadata{};f.disk.readPhysicalPage(30,page);f.disk.readPhysicalPage(0,metadata);
            const auto key=std::tuple{static_cast<unsigned>(mode),static_cast<unsigned>(storage),kind,commit};
            if(policy==RedoPolicy::AlwaysRedo)results[key]={page,metadata};
            else require(results.at(key)==std::pair{page,metadata},"AlwaysRedo/selective final physical bytes differ");
            ++cases;
        }
    std::cout<<"lineage matrix cases="<<cases<<'\n';
}
void negativeEvidence() {
    test::TemporaryDatabase db("lineage_negative");test::ReclaimFixture f(db.path().string());f.initialize();
    static_cast<void>(f.reclaim(30));static_cast<void>(f.reuse(false));
    f.recovery.beginStatement();require(f.allocator.allocatePage()==30,"Negative reuse");initialize(f,30,0,0xBC);
    f.recovery.prepareStatement();f.pool.flushAll();
    const auto original=f.log.scan().records;
    const ReclaimLineageValidator valid(original);
    DiskManager::Page bytes{};f.disk.readPhysicalPage(30,bytes);
    require(valid.provesLater(30,original.front().lsn,bytes),"Positive opaque proof");
    bytes[300]^=std::byte{1};require(!valid.provesLater(30,original.front().lsn,bytes),"Unlogged bytes accepted");
    for(unsigned scenario=0;scenario<3;++scenario) {
        auto records=original;
        auto clr=std::find_if(records.begin(),records.end(),[](const auto& r){return r.type==LogRecordType::Compensation && decodeCompensationLogPayload(r.payload).pageId==30;});
        require(clr!=records.end(),"No target CLR");
        if(scenario==0)records.erase(clr);
        else if(scenario==1)clr->transactionId+=100;
        else {auto payload=decodeCompensationLogPayload(clr->payload);payload.compensatedImage[100]=std::byte{1};clr->payload=encodeCompensationLogPayload(payload);}
        test::requireThrows<WalError>([&]{const ReclaimLineageValidator rejected(records);},"Missing/forged CLR lineage accepted");
    }
    f.recovery.rollbackStatement();
}
void model() {
    std::uint64_t operations=0;
    for(auto seed:{0x123A31U,0xC0FFEEU,0xDE1AU,0xA110CU}) {
        std::mt19937 rng(seed);
        for(unsigned history=0;history<25;++history) {
            test::TemporaryDatabase db("lineage_model");
            const auto mode=static_cast<WalUpdateMode>(history%3);
            auto f=std::make_unique<test::ReclaimFixture>(db.path().string(),WalStorageMode::Segmented,mode);
            RecoveryStats startup;
            f->initialize();static_cast<void>(f->reclaim(30));
            for(unsigned n=0;n<100;++n) {
                try {
                    const unsigned kind=rng()%4;
                    f->recovery.beginStatement();require(f->allocator.allocatePage()==30,"Model identity");initialize(*f,30,kind,rng()%255);
                    f->recovery.prepareStatement();
                    if(n%7==0){f->recovery.commitStatement();f->pool.flushAll();f->live.erase(30);static_cast<void>(f->reclaim(30));}
                    else f->recovery.rollbackStatement();
                    // Three per-page lifecycle events: reuse, transaction
                    // outcome, durable Free publication by reclaim or CLR.
                    operations+=3;verifyFree(*f);
                    if(n%25==24) {
                        f.reset();const auto child=::fork();
                        if(child==0){test::ReclaimFixture interrupted(db.path().string(),WalStorageMode::Segmented,mode);
                            interrupted.recovery.beginStatement();require(interrupted.allocator.allocatePage()==30,"Model crash identity");
                            initialize(interrupted,30,0,0xD3);interrupted.pool.flushAll();interrupted.disk.sync();::_exit(86);}
                        waitCrash(child);
                        const auto replay=::fork();
                        if(replay==0){::setenv("MINIDB_FAILPOINT","recovery_after_clr_wal_force",1);
                            static_cast<void>(recover(db.path().string(),WalStorageMode::Segmented,RedoPolicy::PageLsnSelectiveRedo));::_exit(99);}
                        waitCrash(replay);
                        startup=recover(db.path().string(),WalStorageMode::Segmented,RedoPolicy::PageLsnSelectiveRedo);
                        f=std::make_unique<test::ReclaimFixture>(db.path().string(),WalStorageMode::Segmented,mode);verifyFree(*f);
                    }
                }catch(const std::exception& e){throw std::runtime_error("seed="+std::to_string(seed)+" history="+std::to_string(history)+" step="+std::to_string(n)+": "+e.what());}
            }
            CheckpointControl control(db.path().string()+".ckpt");
            {CheckpointManager checkpoints(f->recovery,f->pool,f->disk,f->log,control,startup);
             for(unsigned c=0;c<4;++c)static_cast<void>(checkpoints.checkpoint(c%2?CheckpointMode::Fuzzy:CheckpointMode::Sharp));
             require(checkpoints.stats().walBytesReclaimed>0,"Model did not reclaim WAL segments");}
            // This model verifies on-disk checkpoint/reopen lineage, not the
            // independent resident-frame retirement issue diagnosed below.
            f.reset();const auto checkpointed=recover(db.path().string(),WalStorageMode::Segmented,RedoPolicy::PageLsnSelectiveRedo);
            require(checkpointed.checkpointUsed,"Durable checkpoint not selected");
            f=std::make_unique<test::ReclaimFixture>(db.path().string(),WalStorageMode::Segmented,mode);verifyFree(*f);
        }
        std::cout<<"model seed="<<seed<<" completed_lifecycle_events=7500\n";
    }
    require(operations==30000,"Model budget not completed");
}
void corruptPhysicalBytes() {
    for(unsigned scenario=0;scenario<3;++scenario) {
        test::TemporaryDatabase db("lineage_bad_physical");
        {test::ReclaimFixture f(db.path().string());f.initialize();static_cast<void>(f.reclaim(30));
         static_cast<void>(f.reuse(false));f.recovery.beginStatement();
         require(f.allocator.allocatePage()==30,"Corruption fixture identity");initialize(f,30,scenario==0?0:1,0xBC);
         f.recovery.commitStatement();f.pool.flushAll();
         DiskManager::Page physical{};f.disk.readPhysicalPage(30,physical);
         if(scenario==2)writePersistentPageLsn(physical,f.log.lastAppendedLsn());
         else physical[300]^=std::byte{1};
         f.disk.writePhysicalPage(30,physical);f.disk.sync();}
        test::requireThrows<WalError>([&]{static_cast<void>(recover(db.path().string(),WalStorageMode::Segmented,RedoPolicy::PageLsnSelectiveRedo));},
            "Unlogged bytes/bogus PageLSN accepted as a later generation");
    }
}
void crashPrefixes() {
    const char* points[]={"before_page_update_append","after_page_update_append","after_page_update_wal_force",
        "after_commit_sync","recovery_after_clr_wal_force","recovery_after_compensation_page_write","recovery_after_abort_fsync",
        "reclaim_after_wal_force","reclaim_after_free_write","reclaim_after_metadata_write",
        "fuzzy_checkpoint_after_control_sync","fuzzy_checkpoint_during_reclamation"};
    unsigned count=0;
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented})
       for(unsigned which=0;which<std::size(points);++which) {
        test::TemporaryDatabase db("lineage_crash_prefix");
        {test::ReclaimFixture f(db.path().string(),storage,mode);f.initialize();static_cast<void>(f.reclaim(30));static_cast<void>(f.reuse(false));}
        const auto child=::fork();
        if(child==0) {try {
            test::ReclaimFixture f(db.path().string(),storage,mode);f.recovery.beginStatement();
            require(f.allocator.allocatePage()==30,"Crash prefix identity");initialize(f,30,0,0xBC);
            if(which<4) {
                ::setenv("MINIDB_FAILPOINT",points[which],1);
                f.recovery.prepareStatement();f.pool.flushAll();f.recovery.commitStatement();
            } else {
                f.recovery.prepareStatement();f.pool.flushAll();f.disk.sync();
                if(which<7){::setenv("MINIDB_FAILPOINT",points[which],1);f.recovery.rollbackStatement();}
                else if(which<10){f.recovery.commitStatement();::setenv("MINIDB_FAILPOINT",points[which],1);static_cast<void>(f.reclaim(30));}
                else {f.recovery.rollbackStatement();
                    CheckpointControl control(db.path().string()+".ckpt");CheckpointManager cp(f.recovery,f.pool,f.disk,f.log,control,{});
                    ::setenv("MINIDB_FAILPOINT",points[which],1);static_cast<void>(cp.checkpoint(CheckpointMode::Fuzzy));}
            }
            ::_exit(99);
        } catch(const std::exception& e){std::cerr<<points[which]<<": "<<e.what()<<'\n';::_exit(98);}}
        waitCrash(child);static_cast<void>(recover(db.path().string(),storage,RedoPolicy::AlwaysRedo));
        static_cast<void>(recover(db.path().string(),storage,RedoPolicy::PageLsnSelectiveRedo));
        test::ReclaimFixture f(db.path().string(),storage,mode);
        if(which==3){require(f.allocator.freePageIds()==std::vector<PageId>({20,10}),"Durable COMMIT lost");
            DiskManager::Page physical{};f.disk.readPhysicalPage(30,physical);require(physical[100]==std::byte{0xBC},"Committed bytes lost");}
        else verifyFree(f);
        ++count;
       }
    std::cout<<"transitive crash prefixes="<<count<<'\n';
}
void linkedLineages() {
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(bool commit:{false,true}) {
        test::TemporaryDatabase db("linked_lineages");
        {test::ReclaimFixture f(db.path().string(),WalStorageMode::Segmented,mode);f.initialize();
         static_cast<void>(f.reclaim(30));static_cast<void>(f.reclaim(40));static_cast<void>(f.reuse(false));}
        const auto child=::fork();
        if(child==0){test::ReclaimFixture f(db.path().string(),WalStorageMode::Segmented,mode);f.recovery.beginStatement();
            require(f.allocator.allocatePage()==40&&f.allocator.allocatePage()==30,"Linked reuse order");
            initialize(f,40,0,0xBB);initialize(f,30,1,0xCC);f.pool.flushAll();f.disk.sync();
            if(commit)f.recovery.commitStatement();
            ::_exit(86);}
        waitCrash(child);static_cast<void>(recover(db.path().string(),WalStorageMode::Segmented,RedoPolicy::PageLsnSelectiveRedo));
        test::ReclaimFixture f(db.path().string(),WalStorageMode::Segmented,mode);
        const auto expected=commit?std::vector<PageId>{20,10}:std::vector<PageId>{40,30,20,10};
        require(f.allocator.freePageIds()==expected,"Interacting lineages corrupted shared head");
        if(commit){auto p=requireReadPage(f.pool,40,"linked winner");require(p.data()[100]==std::byte{0xBB},"Independent winner changed");}
        else for(std::size_t i=0;i<expected.size();++i){DiskManager::Page physical{};f.disk.readPhysicalPage(expected[i],physical);
            require(physical==reclaimedPage({expected[i],i+1<expected.size()?expected[i+1]:INVALID_PAGE_ID,f.disk.pageCount()},readPersistentPageLsn(physical)),"Linked canonical bytes differ");}
      }
}
void interruptedClrReplay() {
    for(auto mode:{WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
      for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented}) {
        test::TemporaryDatabase db("lineage_interrupted_clr_replay");
        {test::ReclaimFixture f(db.path().string(),storage,mode);f.initialize();static_cast<void>(f.reclaim(30));static_cast<void>(f.reuse(false));}
        auto child=::fork();
        if(child==0){test::ReclaimFixture f(db.path().string(),storage,mode);f.recovery.beginStatement();
            require(f.allocator.allocatePage()==30,"Interrupted replay identity");initialize(f,30,0,0xBC);f.pool.flushAll();f.disk.sync();::_exit(86);}
        waitCrash(child);
        child=::fork();
        if(child==0){::setenv("MINIDB_FAILPOINT","recovery_after_redo_page",1);
            static_cast<void>(recover(db.path().string(),storage,RedoPolicy::AlwaysRedo));::_exit(99);}
        waitCrash(child);
        child=::fork();
        if(child==0){::setenv("MINIDB_FAILPOINT","recovery_after_clr_wal_force",1);
            static_cast<void>(recover(db.path().string(),storage,RedoPolicy::PageLsnSelectiveRedo));::_exit(99);}
        waitCrash(child);static_cast<void>(recover(db.path().string(),storage,RedoPolicy::PageLsnSelectiveRedo));
        test::ReclaimFixture f(db.path().string(),storage,mode);verifyFree(f);
    }
}
void frameDiagnostic() {
    test::TemporaryDatabase db("frame_retention_diagnostic");test::ReclaimFixture f(db.path().string());f.initialize();
    static_cast<void>(f.reclaim(30));static_cast<void>(f.reuse(true));f.pool.flushAll();
    CheckpointControl control(db.path().string()+".ckpt");CheckpointManager checkpoints(f.recovery,f.pool,f.disk,f.log,control,{});
    for(unsigned c=0;c<4;++c)static_cast<void>(checkpoints.checkpoint());
    require(checkpoints.stats().walBytesReclaimed>0,"No reclaimed history in diagnostic");
    f.pool.validate(); // Intentionally reports the separate, unfixed invariant.
}
}
int main(int argc,char** argv) {
    try {if(argc==2&&std::string_view(argv[1])=="--model")model();
        else if(argc==2&&std::string_view(argv[1])=="--frame-diagnostic")frameDiagnostic();
        else{originalReproducer();matrix();negativeEvidence();corruptPhysicalBytes();crashPrefixes();linkedLineages();interruptedClrReplay();}
        std::cout<<"Transitive opaque reuse recovery passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
