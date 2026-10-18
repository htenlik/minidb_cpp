#include "page_reclaim_fixture.hpp"
#include <chrono>
#include <iostream>

int main() {
    using namespace minidb;
    try {
        for(auto storage:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented})
          for(unsigned count:{1U,10U,100U,1000U})for(unsigned clrs:{0U,1U,2U}) {
            test::TemporaryDatabase db("lineage_benchmark");
            {test::ReclaimFixture f(db.path().string(),storage);f.initialize();static_cast<void>(f.reclaim(30));
             for(unsigned n=0;n<count;++n){for(unsigned c=0;c<clrs;++c)static_cast<void>(f.reuse(false));
                 static_cast<void>(f.reuse(true));f.pool.flushAll();f.live.erase(30);static_cast<void>(f.reclaim(30));
                 f.recovery.beginStatement();{auto p=requireWritePage(f.pool,60,"unrelated benchmark update");p.data()[100]=static_cast<std::byte>(n%255);}
                 f.recovery.commitStatement();}
             f.pool.flushAll();f.disk.sync();}
            const auto start=std::chrono::steady_clock::now();const auto stats=test::recoverReclaim(db.path().string(),storage);
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"storage="<<(storage==WalStorageMode::Segmented?"segmented":"legacy")<<" cycles="<<count<<" clr_cycles="<<clrs
                <<" recovery_us="<<elapsed<<" records="<<stats.recordsAnalyzed<<" transitions="<<stats.reclaimTransitiveTransitions
                <<" clr_transitions="<<stats.reclaimClrTransitions<<" accounted_lineage_bytes="<<stats.reclaimLineageIndexBytes
                <<" accounted_lsn_index_bytes="<<stats.recoveryLsnIndexBytes
                <<" page_lsn_skips="<<stats.redoSkippedByPageLsn+stats.reclaimRedoSkips<<" writes="<<stats.databaseWrites<<'\n';
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
