#include "page_reclaim_fixture.hpp"
#include <chrono>
#include <iostream>
#include <numeric>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace minidb;
using Clock=std::chrono::steady_clock;
std::uint64_t ns(Clock::time_point start) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()); }
std::uint64_t percentile(std::vector<std::uint64_t> times,unsigned p) {
    std::sort(times.begin(),times.end()); return times[(times.size()*p+99)/100-1];
}
void measure(unsigned count,bool ordinary,bool reuse) {
    test::TemporaryDatabase db("reclaim_cost");
    test::ReclaimFixture f(db.path().string()); f.initialize();
    std::vector<std::uint64_t> times;
    f.log.resetStats();
    const auto started=Clock::now();
    for (unsigned i=0;i<count;++i) {
        const auto page=static_cast<PageId>(i+21);
        const auto start=Clock::now();
        if (ordinary) {
            f.recovery.beginStatement(); f.allocator.releasePage(page); f.recovery.commitStatement();
            f.pool.flushAll(); f.disk.sync(); // equivalent durable data completion, not ordinary release's default promise
        } else static_cast<void>(f.reclaim(page));
        if (reuse) static_cast<void>(f.reuse());
        times.push_back(ns(start));
    }
    const auto total=ns(started);
    const auto statistics=f.log.stats();
    const auto members=f.allocator.freePageIds();
    test::require(members.size()==(reuse?2U:count+2U),"Benchmark free list");
    std::cout << "workload="<<(ordinary?"ordinary-release":reuse?"reclaim-reuse":"system-reclaim")
        << " count="<<count<<" operations_per_second="<<(1.0e9*count/static_cast<double>(total))
        << " p50_ns="<<percentile(times,50)<<" p95_ns="<<percentile(times,95)<<" p99_ns="<<percentile(times,99)
        << " wal_bytes="<<statistics.bytesAppended<<" wal_fsyncs="<<statistics.fsyncCalls
        << " system_page_writes="<<(ordinary?0:2*count)<<" explicit_database_syncs="<<count
        << " direct_metadata_page_writes="<<(ordinary||reuse?count:0)
        << " total_database_page_writes="<<((ordinary?count:2*count)+(reuse?count:0)+f.pool.stats().physicalPageWrites)
        << " buffered_page_writes="<<f.pool.stats().physicalPageWrites<<" free_pages="<<members.size()
        << " physical_pages="<<f.disk.pageCount()<<'\n';
}
void recoveryCost() {
    test::TemporaryDatabase db("reclaim_recovery_cost");
    {test::ReclaimFixture f(db.path().string());f.initialize();}
    const auto child=::fork();
    if (child==0) {test::ReclaimFixture f(db.path().string());::setenv("MINIDB_FAILPOINT","reclaim_after_free_write",1);static_cast<void>(f.reclaim(30));::_exit(99);}
    int status=0;test::require(child>0 && ::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==86,"Benchmark crash");
    const auto start=Clock::now();const auto stats=test::recoverReclaim(db.path().string());const auto elapsed=ns(start);
    std::cout<<"workload=interrupted-recovery recovery_ns="<<elapsed<<" system_page_writes="<<stats.reclaimPageWrites
        <<" pagelsn_skips="<<stats.reclaimRedoSkips<<" database_syncs="<<stats.databaseSyncCalls<<'\n';
}
}
int main() {
    try {measure(20,true,false);for(unsigned n:{1U,10U,100U})measure(n,false,false);measure(20,false,true);recoveryCost();return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
