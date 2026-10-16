#include "page_reclaim_fixture.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/page_lsn.hpp"
#include <iostream>
#include <random>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace minidb;
using test::require;
void crashWait(pid_t child) {
    int status = 0;
    require(child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
        "Hard crash point not reached: " + std::to_string(status));
}
void codec() {
    const PageReclaimPayload value{30,20,128,INVALID_LSN,INVALID_LSN,0x12345678};
    const auto bytes = encodePageReclaim(value);
    require(bytes.size() == 48 && bytes[0] == std::byte{30} && bytes[4] == std::byte{20}
        && bytes[8] == std::byte{1} && bytes[16] == std::byte{128}
        && bytes[24] == std::byte{255} && bytes[40] == std::byte{0x78}
        && bytes[43] == std::byte{0x12}, "Pinned payload layout");
    const LogRecord record{LogRecordType::PageReclaim, 0, INVALID_LSN, bytes, 64};
    const auto encoded = encodeWalRecord(record, 64);
    require(encoded.size() == 96 && decodePageReclaim(decodeWalRecord(encoded,64)) == value, "Record roundtrip");
    require(byte_codec::readUint32(encoded,40) == 2489442473U, "Pinned record CRC32C");
    std::cout << "PAGE_RECLAIM CRC32C=" << byte_codec::readUint32(encoded,40) << '\n';
    for (auto offset : {8U,10U,12U,44U}) {
        auto bad = record; bad.payload[offset] = std::byte{7};
        test::requireThrows<WalError>([&]{ static_cast<void>(decodePageReclaim(bad)); }, "Bad fields accepted");
    }
    for (unsigned length = 0; length < 48; ++length) {
        auto bad = record; bad.payload.resize(length);
        test::requireThrows<WalError>([&]{ static_cast<void>(decodePageReclaim(bad)); }, "Short payload accepted");
    }
    auto bad = record; bad.payload.push_back(std::byte{0});
    test::requireThrows<WalError>([&]{ static_cast<void>(decodePageReclaim(bad)); }, "Trailing payload accepted");
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        bad = record;
        if (scenario == 0) bad.transactionId = 1;
        if (scenario == 1) bad.prevLsn = 1;
        if (scenario == 2) bad.type = LogRecordType::PageReserve;
        test::requireThrows<WalError>([&]{ static_cast<void>(decodePageReclaim(bad)); }, "Bad ownership/type accepted");
    }
}
void directed() {
    test::TemporaryDatabase db("reclaim_directed");
    {
        test::ReclaimFixture f(db.path().string()); f.initialize(); f.disk.updateCatalogRootPageId(5); f.disk.sync();
        const auto before = f.disk.databaseHeader();
        for (PageId page : {30,40,50}) {
            const auto lsn = f.reclaim(page);
            require(f.log.durableLsn() >= lsn && f.pool.pinCount(page).value_or(0) == 0, "Durability/coherence");
            DiskManager::Page p{}, p0{}; f.disk.readPhysicalPage(page,p); f.disk.readPhysicalPage(0,p0);
            require(readPersistentPageLsn(p) == lsn && readPersistentPageLsn(p0) == lsn, "Two PageLSNs");
            test::requireThrows<std::logic_error>([&]{ static_cast<void>(f.reclaim(page)); }, "Duplicate insertion");
        }
        require(f.allocator.freePageIds() == std::vector<PageId>({50,40,30,20,10}), "Canonical chain");
        require(f.disk.databaseHeader().catalogRootPageId == before.catalogRootPageId, "Catalog changed");
        const auto certificate = f.certify(60);
        test::requireThrows<std::invalid_argument>([&]{static_cast<void>(f.reclaim(5));},"Catalog-root reclaim accepted");
        { auto guard = requireReadPage(f.pool,60,"pin");
          test::requireThrows<std::logic_error>([&]{ static_cast<void>(f.reclaimer.reclaim(certificate,f.lease)); }, "Pinned reclaim"); }
        f.live.insert(60);
        test::requireThrows<std::logic_error>([&]{ static_cast<void>(f.certify(60)); }, "Live certification");
        f.recovery.beginStatement();
        test::requireThrows<std::logic_error>([&]{ static_cast<void>(f.reclaimer.reclaim(certificate,f.lease)); }, "Active user reclaim");
        f.recovery.rollbackStatement();
    }
    const auto stats = test::recoverReclaim(db.path().string());
    require(stats.reclaimRecords == 3 && stats.reclaimRedoSkips == 6, "Idempotent replay skips");
    test::ReclaimFixture f(db.path().string());
    require(f.allocator.freePageIds() == std::vector<PageId>({50,40,30,20,10}), "Restart chain");
}
void forceOrder() {
    test::TemporaryDatabase db("reclaim_force_order");
    { test::ReclaimFixture f(db.path().string()); f.initialize();
      ::setenv("MINIDB_THROWPOINT","reclaim_after_wal_force",1);
      test::requireThrows<std::runtime_error>([&]{static_cast<void>(f.reclaim(30));},"Force witness not reached");
      ::unsetenv("MINIDB_THROWPOINT");
      const auto r=f.log.readRecordAt(f.log.lastAppendedLsn());
      require(r.type==LogRecordType::PageReclaim && f.log.durableLsn()>=r.lsn,"Reclaim WAL not durable at pre-write boundary");
      DiskManager::Page before{}; f.disk.readPhysicalPage(30,before);
      require(std::all_of(before.begin(),before.end(),[](std::byte b){return b==std::byte{0};})
          && f.disk.databaseHeader().freeListRootPageId==20,"Data changed before force witness");
      require(!f.recovery.checkpointSafe(),"Incomplete operation permits checkpoint");
      test::requireThrows<std::logic_error>([&]{static_cast<void>(f.recovery.beginTransaction());},"Poisoned operation permits user transaction");
    }
    static_cast<void>(test::recoverReclaim(db.path().string()));
}
void reuse() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
        for (bool commit : {false,true}) {
            test::TemporaryDatabase db("reclaim_reuse");
            { test::ReclaimFixture f(db.path().string(),WalStorageMode::Segmented,mode); f.initialize();
              static_cast<void>(f.reclaim(30)); require(f.reuse(commit) == 30, "Reuse ID"); }
            static_cast<void>(test::recoverReclaim(db.path().string()));
            test::ReclaimFixture f(db.path().string());
            const auto expected = commit ? std::vector<PageId>{20,10} : std::vector<PageId>{30,20,10};
            require(f.allocator.freePageIds() == expected, "Reuse terminal free state");
            if (commit) { auto p = requireReadPage(f.pool,30,"verify reuse");
                require(ConstSlottedPageView(p.data(),30,f.disk.pageCount()).get(0) == TupleBytes(32,std::byte{0xA7}), "Committed reuse lost"); }
        }
    }
}
void crashes() {
    const char* points[] = {"reclaim_before_append","reclaim_after_append","reclaim_after_wal_force",
        "reclaim_after_free_write","reclaim_after_metadata_write","reclaim_before_database_sync","reclaim_after_database_sync"};
    for (auto storage : {WalStorageMode::LegacySingleFile,WalStorageMode::Segmented}) {
        for (bool metadataFirst : {false,true}) for (const auto* point : points) {
            test::TemporaryDatabase db("reclaim_crash");
            { test::ReclaimFixture f(db.path().string(),storage); f.initialize(); }
            const auto child = ::fork();
            if (child == 0) {
                if (metadataFirst) ::setenv("MINIDB_RECLAIM_PAGE0_FIRST","1",1);
                test::ReclaimFixture f(db.path().string(),storage);
                ::setenv("MINIDB_FAILPOINT",point,1); static_cast<void>(f.reclaim(30)); ::_exit(99);
            }
            crashWait(child);
            if (std::string_view(point) == "reclaim_after_wal_force") {
                for (const auto* redoPoint : {"reclaim_after_free_write","reclaim_after_metadata_write"}) {
                    const auto replay = ::fork();
                    if (replay == 0) { ::setenv("MINIDB_FAILPOINT",redoPoint,1);
                        static_cast<void>(test::recoverReclaim(db.path().string(),storage)); ::_exit(99); }
                    crashWait(replay);
                }
            }
            const auto result = test::recoverReclaim(db.path().string(),storage);
            test::ReclaimFixture f(db.path().string(),storage);
            const bool durable = result.reclaimRecords != 0;
            require(f.allocator.freePageIds() == (durable ? std::vector<PageId>{30,20,10} : std::vector<PageId>{20,10}),
                "Crash chain at " + std::string(point));
            if (std::string_view(point) != "reclaim_before_append" && std::string_view(point) != "reclaim_after_append") require(durable,"Forced record absent");
        }
    }
}
void checkpoint() {
    test::TemporaryDatabase db("reclaim_checkpoint");
    { test::ReclaimFixture f(db.path().string()); f.initialize(); static_cast<void>(f.reclaim(30));
      CheckpointControl control(db.path().string()+".ckpt");
      CheckpointManager manager(f.recovery,f.pool,f.disk,f.log,control,{}); manager.attachAccessGate(f.gate);
      for (auto mode : {CheckpointMode::Fuzzy,CheckpointMode::Sharp,CheckpointMode::Sharp,CheckpointMode::Sharp,CheckpointMode::Sharp})
          static_cast<void>(manager.checkpoint(mode,f.lease));
      require(f.log.stats().segmentsDeleted>0,"Completed reclaim WAL was never reclaimed");
    }
    std::filesystem::remove(db.path().string()+".ckpt");
    static_cast<void>(test::recoverReclaim(db.path().string()));
    test::ReclaimFixture f(db.path().string());
    require(f.allocator.freePageIds() == std::vector<PageId>({30,20,10}),"Lost control/reclamation");
}
void corruption() {
    for (unsigned scenario=0;scenario<8;++scenario) {
        test::TemporaryDatabase db("reclaim_corrupt");
        { test::ReclaimFixture f(db.path().string()); f.initialize();
          const auto r = f.reclaim(30);
          if (scenario < 5) {
              auto record = f.log.readRecordAt(r);
              if (scenario == 0) record.payload[0] = std::byte{0};
              if (scenario == 1) byte_codec::writeUint32(record.payload,page_reclaim_layout::HEAD,128);
              if (scenario == 2) record.payload[page_reclaim_layout::FLAGS] = std::byte{1};
              if (scenario == 3) record.transactionId = 1;
              if (scenario == 4) { // buried duplicate, with valid current before-state
                  static_cast<void>(f.reclaim(40));
                  DiskManager::Page page{}; f.disk.readPhysicalPage(30,page);
                  record.payload = encodePageReclaim({30,40,f.disk.pageCount(),readPersistentPageLsn(page),
                      f.log.lastAppendedLsn(),crc32c(page)});
              }
              static_cast<void>(f.log.append(record)); f.log.flushAll();
          } else {
              DiskManager::Page page{}; f.disk.readPhysicalPage(scenario==5 ? 0 : 30,page);
              if (scenario==5) {
                  auto header = database_format::deserializeDatabaseHeader(page);
                  header.freeListRootPageId = 20; database_format::serializeDatabaseHeader(header,page);
                  writePersistentPageLsn(page,r);
              } else if (scenario==6) page[free_page_layout::HEADER_SIZE] = std::byte{1};
              else byte_codec::writeUint32(page,free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,30);
              f.disk.writePhysicalPage(scenario==5 ? 0 : 30,page); f.disk.sync();
          }
        }
        test::requireThrows<std::runtime_error>([&]{static_cast<void>(test::recoverReclaim(db.path().string()));},"Corrupt reclaim history accepted");
    }
}
void unfinishedReuse() {
    for (auto mode : {WalUpdateMode::FullPage,WalUpdateMode::ByteRange,WalUpdateMode::Adaptive})
    for (bool typed : {false,true}) {
    test::TemporaryDatabase db("reclaim_loser_reuse");
    { test::ReclaimFixture f(db.path().string()); f.initialize(); static_cast<void>(f.reclaim(30)); }
    const auto child = ::fork();
    if (child==0) {
        test::ReclaimFixture f(db.path().string(),WalStorageMode::Segmented,mode); f.recovery.beginStatement();
        const auto page = f.allocator.allocatePage();
        { auto p = requireWritePage(f.pool,page,"loser reuse");
          if (typed) SlottedPageView::initialize(p.data(),page,f.disk.pageCount(),127); }
        f.recovery.prepareStatement(); f.pool.flushAll(); f.disk.sync(); ::_exit(86);
    }
    crashWait(child);
    const auto stats = test::recoverReclaim(db.path().string());
    require(stats.loserTransactions==1 && stats.clrsAppended>=2,"Reuse loser did not use CLR UNDO");
    require(stats.reclaimRedoSkips==2,"Old reclaim overwrote newer typed/opaque reuse state");
    test::ReclaimFixture f(db.path().string());
    require(f.allocator.freePageIds()==std::vector<PageId>({30,20,10}),"Reclaim was undone by loser reuse");
    }
}
void model() {
    std::uint64_t count = 0;
    for (auto seed : {0x123A3U,0xC0FFEEU,0xDE1AU,0xA110CU}) {
        std::mt19937 rng(seed);
        for (unsigned history=0; history<25; ++history) {
            test::TemporaryDatabase db("reclaim_model");
            std::vector<PageId> expected{20,10}, pages;
            for (PageId p=1; pages.size()<100; ++p) if (p!=10 && p!=20) pages.push_back(p);
            std::shuffle(pages.begin(),pages.end(),rng);
            { auto f=std::make_unique<test::ReclaimFixture>(db.path().string()); f->initialize();
              unsigned operation=0;
              for (auto p : pages) {
                  if (operation++%10==0) {
                      const char* points[]={"reclaim_before_append","reclaim_after_append","reclaim_after_wal_force",
                          "reclaim_after_free_write","reclaim_after_metadata_write","reclaim_before_database_sync","reclaim_after_database_sync"};
                      const auto selected=rng()%7;
                      const auto child=::fork();
                      if (child==0) {::setenv("MINIDB_FAILPOINT",points[selected],1);static_cast<void>(f->reclaim(p));::_exit(99);}
                      crashWait(child); f.reset();
                      if (selected==2) {
                          const auto replay=::fork();
                          if (replay==0) {::setenv("MINIDB_FAILPOINT","reclaim_after_free_write",1);static_cast<void>(test::recoverReclaim(db.path().string()));::_exit(99);}
                          crashWait(replay);
                      }
                      static_cast<void>(test::recoverReclaim(db.path().string()));
                      f=std::make_unique<test::ReclaimFixture>(db.path().string());
                      if (selected<2) static_cast<void>(f->reclaim(p));
                  } else static_cast<void>(f->reclaim(p));
                  expected.insert(expected.begin(),p); ++count;
                  require(f->allocator.freePageIds()==expected,"Model prefix seed="+std::to_string(seed)+" history="+std::to_string(history));
              }
              require(f->allocator.freePageIds() == expected,"Model reclaim chain");
              for (unsigned i=0;i<100;++i) {
                  const bool commit = rng()%3 != 0;
                  const auto page = f->reuse(commit);
                  require(page == expected.front(),"Model reuse page");
                  if (commit) expected.erase(expected.begin());
                  ++count;
              }
            }
            static_cast<void>(test::recoverReclaim(db.path().string()));
            { test::ReclaimFixture f(db.path().string());
              require(f.allocator.freePageIds() == expected,"Model restart seed="+std::to_string(seed)+" history="+std::to_string(history)); }
        }
    }
    require(count==20000,"Model count");
    std::cout << "reclaim_model operations=20000 hard_crashed_attempts=1000 seeds=0x123A3,0xC0FFEE,0xDE1A,0xA110C\n";
}
}
int main() {
    try { codec(); directed(); forceOrder(); reuse(); unfinishedReuse(); crashes(); checkpoint(); corruption(); model(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
