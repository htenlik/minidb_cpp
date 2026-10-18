#include "page_reclaim_fixture.hpp"
#include <iostream>
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
}
int main() {
    try {originalReproducer();std::cout<<"Transitive opaque reuse recovery passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
