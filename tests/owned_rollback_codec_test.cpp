#include "minidb/owned_rollback_analysis.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/log_manager.hpp"
#include "test_utils.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace minidb;
using test::require;
template<class F> void rejects(F function) {
    test::requireThrows<WalError>(function, "Unsafe owned WAL evidence was accepted");
}

// Build through the real outer codec; no fake LSN spacing or alternate scanner.
struct History {
    std::vector<LogRecord> records;
    Lsn end = wal_file_layout::HEADER_SIZE;
    std::map<TransactionId, Lsn> last;
    Lsn add(LogRecordType type, std::vector<std::byte> payload = {}, TransactionId owner = 7) {
        const bool begin = type == LogRecordType::Begin || type == LogRecordType::BeginOwned;
        const auto prev = owner == 0 || begin ? INVALID_LSN : last.at(owner);
        auto bytes = encodeWalRecord({type, owner, prev, std::move(payload)}, end);
        records.push_back(decodeWalRecord(bytes, end));
        const auto lsn = end;
        end += bytes.size();
        if (owner != 0) last[owner] = lsn;
        return lsn;
    }
    Lsn begin(bool owned = true, TransactionId owner = 7, std::uint64_t extent = 10) {
        return add(owned ? LogRecordType::BeginOwned : LogRecordType::Begin,
            owned ? encodeOwnedBegin({extent}) : encodeBeginLogPayload({extent}), owner);
    }
    Lsn reserve(PageId page = 10, TransactionId owner = 7) {
        return add(LogRecordType::PageReserve, encodePageReserveLogPayload({page, PageAllocationSource::Appended, page}), owner);
    }
    Lsn cleanup(Lsn beginLsn, std::uint64_t count = 1) {
        return add(LogRecordType::RollbackCleanupBegin, encodeCleanupBegin({beginLsn, count}));
    }
};

void exactCodecs() {
    // Exact published prototype vectors: IDs, payloads, offsets, CRC and widths.
    const std::array<LogRecord, 4> records{{
        {LogRecordType::BeginOwned,42,INVALID_LSN,encodeOwnedBegin({101}),64},
        {LogRecordType::RollbackCleanupBegin,42,64,encodeCleanupBegin({64,1}),136},
        {LogRecordType::RollbackCleanupIntent,42,136,
            encodeCleanupIntent({120,328,{101,INVALID_PAGE_ID,103,INVALID_LSN,INVALID_LSN,0x12345678}}),208},
        {LogRecordType::RollbackCleanupProgress,42,208,encodeCleanupProgress({120,328,101,103}),424},
    }};
    const std::array<std::uint32_t,4> crcs{197673803U,1279510934U,3953176807U,328734107U};
    const std::array<std::size_t,4> sizes{24,24,72,40};
    for (std::size_t i=0; i<records.size(); ++i) {
        const auto& r = records[i];
        require(static_cast<unsigned>(r.type) == 15+i && r.payload.size() == sizes[i], "Prototype type/width changed");
        validateTransactionRecordPayload(r);
        const auto outer = encodeWalRecord(r,r.lsn);
        require(byte_codec::readUint32(outer,wal_record_layout::CHECKSUM_OFFSET) == crcs[i], "Prototype CRC changed");
        require(decodeWalRecord(outer,r.lsn) == r, "Outer round trip");
        for (std::size_t size=0; size<=r.payload.size()+1; ++size) {
            if (size==r.payload.size()) continue;
            auto bad=r; bad.payload.resize(size);
            rejects([&]{validateTransactionRecordPayload(bad);});
        }
        // Every byte of the version, size and flags must be checked.
        for (std::size_t offset=0; offset<8; ++offset) {
            auto bad=r; bad.payload[offset]^=std::byte{2};
            rejects([&]{validateTransactionRecordPayload(bad);});
        }
        {
            auto bad=r; bad.transactionId=0;
            rejects([&]{validateTransactionRecordPayload(bad);});
        }
        for (auto previous : {Lsn{0},r.lsn,r.lsn+1,INVALID_LSN}) {
            if (i==0 && previous==INVALID_LSN) continue;
            auto bad=r; bad.prevLsn=previous;
            rejects([&]{validateTransactionRecordPayload(bad);});
        }
        auto bad=r; bad.lsn=INVALID_LSN-1;
        rejects([&]{validateTransactionRecordPayload(bad);});
        auto bytes=outer; byte_codec::writeUint16(bytes,wal_record_layout::TYPE_OFFSET,19);
        rejects([&]{static_cast<void>(decodeWalRecord(bytes,r.lsn));});
        bytes=outer; byte_codec::writeUint32(bytes,wal_record_layout::TOTAL_LENGTH_OFFSET,1);
        rejects([&]{static_cast<void>(decodeWalRecord(bytes,r.lsn));});
    }
    require(records[0].payload[8]==std::byte{101} && records[0].payload[9]==std::byte{0}
        && records[0].payload[2]==std::byte{24} && byte_codec::readUint64(records[2].payload,16)==328
        && byte_codec::readUint32(records[2].payload,24)==101
        && byte_codec::readUint32(records[2].payload,64)==0x12345678, "Little-endian offsets");
    require(encodeOwnedBegin(decodeOwnedBegin(records[0]))==records[0].payload
        && encodeCleanupBegin(decodeCleanupBegin(records[1]))==records[1].payload
        && encodeCleanupIntent(decodeCleanupIntent(records[2]))==records[2].payload
        && encodeCleanupProgress(decodeCleanupProgress(records[3]))==records[3].payload, "Payload round trips");
    for (auto index : {0U,2U,3U}) {
        std::vector<std::size_t> offsets;
        if(index==0) for(std::size_t n=16;n<24;++n) offsets.push_back(n);
        if(index==2) offsets={34,35,36,37,38,39,68,69,70,71};
        if(index==3) offsets={28,29,30,31};
        for(auto offset:offsets) {
            auto bad=records[index]; bad.payload[offset]=std::byte{1};
            rejects([&]{validateTransactionRecordPayload(bad);});
        }
    }
    for(auto count:{std::uint64_t{0},std::uint64_t{INVALID_PAGE_ID}+1}) {
        rejects([&]{static_cast<void>(encodeOwnedBegin({count}));});
        auto bad=records[0];byte_codec::writeUint64(bad.payload,8,count);
        rejects([&]{static_cast<void>(decodeOwnedBegin(bad));});
    }
    rejects([]{static_cast<void>(encodeCleanupBegin({INVALID_LSN,1}));});
    rejects([]{static_cast<void>(encodeCleanupBegin({64,INVALID_PAGE_ID}));});
    rejects([]{static_cast<void>(cleanupReclaimLsn(INVALID_LSN-120));});
    for(auto offset:{8U,16U}) {
        auto bad=records[2];byte_codec::writeUint64(bad.payload,offset,bad.lsn);
        rejects([&]{static_cast<void>(decodeCleanupIntent(bad));});
    }
    for(auto page:{PageId{0},INVALID_PAGE_ID,PageId{103}}) {
        auto bad=records[2];byte_codec::writeUint32(bad.payload,24,page);
        rejects([&]{static_cast<void>(decodeCleanupIntent(bad));});
    }
    for(auto offset:{32U,40U,48U,56U}) {
        auto bad=records[2];
        if(offset==32) byte_codec::writeUint16(bad.payload,offset,2);
        else byte_codec::writeUint64(bad.payload,offset,offset==40?0:328);
        rejects([&]{static_cast<void>(decodeCleanupIntent(bad));});
    }
    rejects([]{static_cast<void>(encodeCleanupProgress({64,128,0,10}));});
    rejects([]{static_cast<void>(encodeCleanupProgress({64,128,10,10}));});
    rejects([]{static_cast<void>(encodeCleanupProgress({64,INVALID_LSN,9,10}));});
    rejects([]{static_cast<void>(encodeCleanupIntent({INVALID_LSN,328,{101,INVALID_PAGE_ID,103}}));});
}

History completed(bool absent=false) {
    History h; const auto begin=h.begin();const auto reserve=h.reserve();h.cleanup(begin);
    if(absent) {
        h.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({reserve,INVALID_LSN,10,10}));
    } else {
        const auto reclaim=cleanupReclaimLsn(h.end);
        const PageReclaimPayload v{10,INVALID_PAGE_ID,11,INVALID_LSN,INVALID_LSN,0xAB};
        h.add(LogRecordType::RollbackCleanupIntent,encodeCleanupIntent({reserve,reclaim,v}));
        h.add(LogRecordType::PageReclaim,encodePageReclaim(v),0);
        h.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({reserve,reclaim,10,11}));
    }
    h.add(LogRecordType::Abort);
    return h;
}
void validHistories() {
    for(bool absent:{false,true}) {
        auto h=completed(absent);
        const auto t=analyzeOwnedRollbackHistory(h.records).at(7);
        require(t.protocol==RollbackProtocol::OwnedAppend && t.phase==OwnedCleanupPhase::AbortRecorded
            && t.logicalUndoChainComplete && !t.reservations.at(10).reservation.materializedInRuntime,
            "Owned analysis confused intent with physical materialization");
        require(t.reservations.at(10).outcome==(absent?ReservationCleanupOutcome::AbsenceRecorded
            :ReservationCleanupOutcome::ReclaimRecorded),"Completion kind");
        // Prefixes include BEGIN-only, incomplete intent, reclaim without
        // progress, and complete progress without ABORT. All remain unfinished.
        for(std::size_t n=1;n<h.records.size();++n) {
            auto prefix=analyzeOwnedRollbackHistory(std::span(h.records).first(n)).at(7);
            require(prefix.phase!=OwnedCleanupPhase::AbortRecorded,"Premature completion");
        }
    }
    History legacy;legacy.begin(false);legacy.reserve();legacy.add(LogRecordType::Abort);
    require(analyzeOwnedRollbackHistory(legacy.records).at(7).protocol==RollbackProtocol::LegacyTruncate,"Legacy protocol changed");
    History mixed; const auto begin=mixed.begin(); const auto reserve=mixed.reserve();
    mixed.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({10,false,{},{}}));
    const auto update=mixed.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({2,true,{},{}}));
    const auto prev=mixed.records.back().prevLsn;
    mixed.add(LogRecordType::Compensation,encodeCompensationLogPayload({2,true,false,prev,update,{}}));
    mixed.cleanup(begin);
    mixed.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({reserve,INVALID_LSN,10,10}));
    mixed.add(LogRecordType::Abort);
    const auto t=analyzeOwnedRollbackHistory(mixed.records).at(7);
    require(t.preexistingUpdates.contains(2) && t.appendedUpdates.contains(10),"Existing/append evidence conflated");
    // Separate serialized transactions may reuse the same existing physical ID.
    History serial;serial.begin(false,1);serial.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({2,true,{},{}}),1);
    serial.add(LogRecordType::Commit,{},1);serial.begin(true,2);
    serial.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({2,true,{},{}}),2);serial.add(LogRecordType::Commit,{},2);
    require(analyzeOwnedRollbackHistory(serial.records).size()==2,"Serialized ownership not released at terminal boundary");
    History empty;const auto emptyBegin=empty.begin();empty.cleanup(emptyBegin,0);empty.add(LogRecordType::Abort);
    require(analyzeOwnedRollbackHistory(empty.records).at(7).logicalUndoChainComplete,"Empty cleanup");
    for(auto type:{LogRecordType::PageUpdate,LogRecordType::PageDeltaUpdate,
                  LogRecordType::PageUpdateV2,LogRecordType::PageDeltaUpdateV2}) {
        History chain;const auto start=chain.begin();const auto reserved=chain.reserve();
        std::vector<std::byte> payload;
        const std::vector<PageByteRange> ranges{{100,1,{std::byte{0}},{std::byte{1}}}};
        if(type==LogRecordType::PageUpdate)payload=encodePageUpdateLogPayload({2,true,{},{}});
        else if(type==LogRecordType::PageDeltaUpdate)payload=encodePageDeltaUpdateLogPayload({2,true,ranges});
        else if(type==LogRecordType::PageUpdateV2)payload=encodePageUpdateV2LogPayload({2,true,INVALID_LSN,{},{}});
        else payload=encodePageDeltaUpdateV2LogPayload({2,true,INVALID_LSN,ranges});
        const auto update=chain.add(type,payload);
        chain.add(LogRecordType::Compensation,encodeCompensationLogPayload({2,true,false,reserved,update,{}}));
        chain.cleanup(start);
        chain.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({reserved,INVALID_LSN,10,10}));
        chain.add(LogRecordType::Abort);
        require(analyzeOwnedRollbackHistory(chain.records).at(7).phase==OwnedCleanupPhase::AbortRecorded,"Update/CLR variant chain");
        // The same original update must not be compensated a second time.
        chain.records.resize(4);chain.end=chain.records.back().lsn+wal_record_layout::HEADER_SIZE+chain.records.back().payload.size();
        chain.last[7]=chain.records.back().lsn;
        chain.add(LogRecordType::Compensation,encodeCompensationLogPayload({2,true,false,reserved,update,{}}));
        rejects([&]{static_cast<void>(analyzeOwnedRollbackHistory(chain.records));});
    }
}

void invalidHistories() {
    const auto invalid=[](const History& h){rejects([&]{static_cast<void>(analyzeOwnedRollbackHistory(h.records));});};
    auto h=completed();h.records[2].transactionId=8;invalid(h);
    h=completed();h.records[2].prevLsn++;invalid(h);
    h=completed();h.records[0].type=LogRecordType::Begin;h.records[0].payload=encodeBeginLogPayload({10});invalid(h);
    h=completed();h.records[2].payload=encodeCleanupBegin({h.records[1].lsn,1});invalid(h);
    h=completed();h.records[2].payload=encodeCleanupBegin({h.records[0].lsn,2});invalid(h);
    h=completed();h.records[3].payload=encodeCleanupIntent({h.records[0].lsn,cleanupReclaimLsn(h.records[3].lsn),
        {10,INVALID_PAGE_ID,11,INVALID_LSN,INVALID_LSN,0xAB}});invalid(h);
    h=completed();h.records[4].payload=encodePageReclaim({10,INVALID_PAGE_ID,11,INVALID_LSN,INVALID_LSN,0xAC});invalid(h);
    h=completed();h.records[5].payload=encodeCleanupProgress({h.records[0].lsn,h.records[4].lsn,10,11});invalid(h);
    h=completed();h.records[5].payload=encodeCleanupProgress({h.records[1].lsn,h.records[4].lsn,10,12});invalid(h);
    h=completed();h.add(LogRecordType::Commit);invalid(h);
    h=completed();h.records.back().lsn++;invalid(h);
    for(unsigned scenario=0;scenario<13;++scenario) {
        History bad;const auto begin=bad.begin();const auto reserve=bad.reserve();
        switch(scenario) {
        case 0:bad.reserve();break;
        case 1:bad.add(LogRecordType::Abort);break;
        case 2:bad.cleanup(begin);bad.cleanup(begin);break;
        case 3:bad.cleanup(begin);bad.add(LogRecordType::Commit);break;
        case 4:bad.cleanup(begin);bad.add(LogRecordType::RollbackCleanupProgress,
            encodeCleanupProgress({reserve,bad.records.back().lsn,10,11}));break;
        case 5:bad.cleanup(begin);bad.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({reserve,INVALID_LSN,10,10}));
            bad.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({reserve,INVALID_LSN,10,10}));break;
        case 6:bad.begin(true,8);bad.reserve(10,8);break;
        case 7:bad.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({11,false,{},{}}));break;
        case 8:bad.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({2,false,{},{}}));break;
        case 9:bad.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({2,true,{},{}}));bad.cleanup(begin);break;
        case 10:bad.cleanup(begin);bad.reserve(11);break;
        case 11:bad.begin();break;
        case 12:bad.cleanup(begin);bad.add(LogRecordType::RollbackCleanupIntent,encodeCleanupIntent({reserve,cleanupReclaimLsn(bad.end),
                {10,INVALID_PAGE_ID,11}}));bad.add(LogRecordType::Abort);break;
        }
        invalid(bad);
    }
    History missing;missing.begin(true,1);missing.last[7]=64;missing.reserve();invalid(missing);
    History premature;premature.begin();premature.add(LogRecordType::RollbackCleanupProgress,encodeCleanupProgress({64,INVALID_LSN,10,10}));invalid(premature);
    History badClr;badClr.begin();const auto a=badClr.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({2,true,{},{}}));
    badClr.add(LogRecordType::PageUpdate,encodePageUpdateLogPayload({3,true,{},{}}));
    badClr.add(LogRecordType::Compensation,encodeCompensationLogPayload({2,true,false,64,a,{}}));invalid(badClr);
    auto count=completed();byte_codec::writeUint64(count.records[1].payload,8,9);invalid(count);
    auto duplicate=completed();duplicate.records.resize(5);duplicate.last[7]=duplicate.records[3].lsn;
    duplicate.end=duplicate.records.back().lsn+wal_record_layout::HEADER_SIZE+duplicate.records.back().payload.size();
    duplicate.add(LogRecordType::RollbackCleanupIntent,encodeCleanupIntent({duplicate.records[1].lsn,
        cleanupReclaimLsn(duplicate.end),{10,INVALID_PAGE_ID,11}}));invalid(duplicate);
    History legacyCleanup;const auto legacyBegin=legacyCleanup.begin(false);legacyCleanup.reserve();
    legacyCleanup.cleanup(legacyBegin);invalid(legacyCleanup);
    auto source=completed();source.records[1].payload[page_reserve_log_layout::SOURCE_OFFSET]=std::byte{2};invalid(source);
    auto beforeLsn=completed();byte_codec::writeUint64(beforeLsn.records[3].payload,48,beforeLsn.records[3].lsn);invalid(beforeLsn);
}

void scannerAndInactiveRecovery() {
    for(auto mode:{WalStorageMode::LegacySingleFile,WalStorageMode::Segmented}) {
        test::TemporaryDatabase db("owned_codec_scanner");
        const auto h=completed();
        {LogManager log(walPathForDatabase(db.path().string()),4096,LogOpenMode::EagerValidated,mode,512);
         for(const auto& r:h.records)require(log.append(r)==r.lsn,"Real append LSN changed");log.flushAll();}
        {LogManager log(walPathForDatabase(db.path().string()),4096,LogOpenMode::DeferredRecovery,mode,512);
         const auto scan=log.scan();require(scan.records==h.records && !scan.truncatedTail,"Real scanner round trip");
         require(analyzeOwnedRollbackHistory(scan.records).at(7).phase==OwnedCleanupPhase::AbortRecorded,"Scanner analysis");
         DiskManager disk(db.path().string());const auto before=disk.pageCount();
         rejects([&]{static_cast<void>(RecoveryManager(disk,log).recover());});
         require(disk.pageCount()==before && log.scan().records==h.records,"Inactive recovery performed cleanup");}
    }
    // Every torn-tail prefix: real scanner returns only complete records. No
    // missing system record is manufactured and no progress is inferred.
    const auto h=completed();
    for(std::size_t index=0;index<h.records.size();++index) {
        const auto encoded=encodeWalRecord(h.records[index],h.records[index].lsn);
        for(auto length:{std::size_t{1},wal_record_layout::HEADER_SIZE-1,encoded.size()-1}) {
            test::TemporaryDatabase db("owned_codec_tail");const auto path=walPathForDatabase(db.path().string());
            {std::ofstream file(path,std::ios::binary);const auto header=encodeWalFileHeader();
             file.write(reinterpret_cast<const char*>(header.data()),header.size());
             for(std::size_t n=0;n<index;++n){const auto bytes=encodeWalRecord(h.records[n],h.records[n].lsn);
                 file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
             file.write(reinterpret_cast<const char*>(encoded.data()),static_cast<std::streamsize>(length));}
            const auto scan=scanWalFile(path);
            require(scan.truncatedTail && scan.records.size()==index,"Torn tail was not kept incomplete");
            static_cast<void>(analyzeOwnedRollbackHistory(scan.records));
        }
    }
    // A future record ID is a hard scanner rejection, not a repairable tail.
    {test::TemporaryDatabase db("owned_codec_future");const auto path=walPathForDatabase(db.path().string());
     auto bytes=encodeWalRecord(h.records.front(),h.records.front().lsn);
     byte_codec::writeUint16(bytes,wal_record_layout::TYPE_OFFSET,19);
     {std::ofstream file(path,std::ios::binary);const auto header=encodeWalFileHeader();
      file.write(reinterpret_cast<const char*>(header.data()),header.size());
      file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
     rejects([&]{static_cast<void>(scanWalFile(path));});}
    // Raw unleased coordinator use retains legacy BEGIN; confirmation differs from
    // restart analysis's unknown materialization (without an activation flag).
    test::TemporaryDatabase db("owned_inactive_production");DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));RecoveryCoordinator recovery(disk,log);
    BufferPoolManager pool(disk,4,2,&log,&recovery);recovery.attachBufferPool(pool);recovery.beginStatement();
    {auto page=pool.newPageWrite();require(page.has_value(),"Production append");}
    const auto runtime=recovery.transactionSnapshot().front();
    require(runtime.reservations.at(1).materializedInRuntime,"Append confirmation missing");
    const auto scan=log.scan();require(scan.records.front().type==LogRecordType::Begin,"Raw coordinator changed legacy BEGIN");
    const auto analyzed=analyzeOwnedRollbackHistory(scan.records).at(runtime.transactionId);
    require(!analyzed.reservations.at(1).reservation.materializedInRuntime,"Restart fabricated materialization");
    recovery.rollbackStatement();require(disk.pageCount()==1,"Legacy rollback semantics changed");
}
}
int main() {
    try {exactCodecs();validHistories();invalidHistories();scannerAndInactiveRecovery();
        std::cout<<"Owned rollback codecs and analysis passed (physical execution inactive)\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
