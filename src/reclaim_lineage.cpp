#include "minidb/reclaim_lineage.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_lsn.hpp"
#include <algorithm>
#include <optional>
#include <set>

namespace minidb {
namespace {
using Page = DiskManager::Page;
[[noreturn]] void bad(PageId page, Lsn lsn, const char* reason) {
    throw WalError(WalErrorKind::CorruptRecord,"Reclaim lineage page="+std::to_string(page)
        +" LSN="+std::to_string(lsn)+": "+reason);
}
std::optional<PageId> affected(const LogRecord& r) {
    switch(r.type) {
    case LogRecordType::PageReclaim:return decodePageReclaim(r).page;
    case LogRecordType::PageUpdate:return decodePageUpdateLogPayload(r.payload).pageId;
    case LogRecordType::PageDeltaUpdate:return decodePageDeltaUpdateLogPayload(r.payload).pageId;
    case LogRecordType::PageUpdateV2:return decodePageUpdateV2LogPayload(r.payload).pageId;
    case LogRecordType::PageDeltaUpdateV2:return decodePageDeltaUpdateV2LogPayload(r.payload).pageId;
    case LogRecordType::Compensation:return decodeCompensationLogPayload(r.payload).pageId;
    default:return std::nullopt;
    }
}
Page normalized(Page page) {
    if(supportsPersistentPageLsn(page)) clearPersistentPageLsn(page);
    return page;
}
bool rangesMatch(const Page& base,const std::vector<PageByteRange>& ranges) {
    return std::all_of(ranges.begin(),ranges.end(),[&](const auto& range){
        return std::equal(range.beforeBytes.begin(),range.beforeBytes.end(),base.begin()+range.offset);
    });
}
}

DiskManager::Page ReclaimLineageValidator::after(const Transition& t) {
    const auto& r=*t.record;
    if(r.type==LogRecordType::PageReclaim)return reclaimedPage(decodePageReclaim(r),r.lsn);
    Page result{};
    if(r.type==LogRecordType::PageUpdate) return decodePageUpdateLogPayload(r.payload).afterImage;
    if(r.type==LogRecordType::PageUpdateV2)result=decodePageUpdateV2LogPayload(r.payload).afterImage;
    else if(r.type==LogRecordType::Compensation) {
        const auto clr=decodeCompensationLogPayload(r.payload);result=clr.compensatedImage;
        if(!clr.pageSupportsLsn)return result;
    } else {
        result=*t.transactionBase;
        if(r.type==LogRecordType::PageDeltaUpdate) {
            applyPageDeltaAfter(result,decodePageDeltaUpdateLogPayload(r.payload));return result;
        }
        result=normalized(result);
        const auto delta=decodePageDeltaUpdateV2LogPayload(r.payload);
        applyPageDeltaAfter(result,{delta.pageId,delta.beforePageExisted,delta.ranges});
    }
    if(!supportsPersistentPageLsn(result))throw WalError(WalErrorKind::CorruptRecord,"Lineage V2/CLR after-image lacks its declared PageLSN slot");
    writePersistentPageLsn(result,r.lsn);return result;
}

ReclaimLineageValidator::ReclaimLineageValidator(const std::vector<LogRecord>& records) {
    // Index once. Unrelated WAL does not get scanned again for every reclaim.
    std::map<PageId,std::vector<const LogRecord*>> index;
    for(const auto& r:records)if(r.type==LogRecordType::PageReclaim)index.try_emplace(decodePageReclaim(r).page);
    for(const auto& r:records) {
        const auto page=affected(r);
        if(page && index.contains(*page))index.at(*page).push_back(&r);
    }
    for(const auto& [page,history]:index) {
        std::optional<Page> current;
        TransactionId owner=0;
        std::shared_ptr<const Page> base;
        auto& lineage=pages_[page];
        for(const auto* record:history) {
            const auto& r=*record;
            if(!current && r.type!=LogRecordType::PageReclaim)continue;
            if(r.type==LogRecordType::PageReclaim) {
                const auto reclaim=decodePageReclaim(r);
                if(current && (crc32c(*current)!=reclaim.beforeCrc
                    || readPersistentPageLsn(*current)!=reclaim.beforePageLsn
                    || std::equal(free_page_layout::MAGIC.begin(),free_page_layout::MAGIC.end(),current->begin())))
                    bad(page,r.lsn,"duplicate reclaim or incompatible generation pre-image");
                owner=0;base.reset();
            } else {
                if(owner!=r.transactionId) {
                    owner=r.transactionId;base=std::make_shared<const Page>(*current);
                    stats_.accountedBytes+=sizeof(Page);
                }
                bool matches=false;
                switch(r.type) {
                case LogRecordType::PageUpdate: {
                    const auto v=decodePageUpdateLogPayload(r.payload);
                    matches=v.beforePageExisted && v.beforeImage==*base;break;
                }
                case LogRecordType::PageDeltaUpdate: {
                    const auto v=decodePageDeltaUpdateLogPayload(r.payload);
                    matches=v.beforePageExisted && rangesMatch(*base,v.ranges);break;
                }
                case LogRecordType::PageUpdateV2: {
                    const auto v=decodePageUpdateV2LogPayload(r.payload);
                    matches=v.beforePageExisted && v.beforePageLsn==readPersistentPageLsn(*base)
                        && v.beforeImage==normalized(*base);break;
                }
                case LogRecordType::PageDeltaUpdateV2: {
                    const auto v=decodePageDeltaUpdateV2LogPayload(r.payload);
                    matches=v.beforePageExisted && v.beforePageLsn==readPersistentPageLsn(*base)
                        && rangesMatch(normalized(*base),v.ranges);break;
                }
                case LogRecordType::Compensation: {
                    const auto v=decodeCompensationLogPayload(r.payload);
                    const auto original=lineage.states.find(v.compensatedUpdateLsn);
                    matches=original!=lineage.states.end()
                        && original->second.record->transactionId==r.transactionId
                        && original->second.record->type!=LogRecordType::Compensation
                        && original->second.record->type!=LogRecordType::PageReclaim
                        && original->second.record->prevLsn==v.undoNextLsn
                        && v.pageSupportsLsn==supportsPersistentPageLsn(*base)
                        && v.compensatedImage==normalized(*base);
                    if(matches)++stats_.clrs;break;
                }
                default:break;
                }
                if(!matches)bad(page,r.lsn,"missing/inconsistent transaction before-state or CLR ancestor");
                ++stats_.transitions;
            }
            Transition transition{record,base};
            current=after(transition);
            lineage.states.emplace(r.lsn,std::move(transition));
            if(!supportsPersistentPageLsn(*current)) {
                lineage.opaqueByCrc[crc32c(*current)].push_back(r.lsn);++stats_.opaque;
                stats_.accountedBytes+=sizeof(Lsn);
            }
            stats_.accountedBytes+=sizeof(decltype(lineage.states)::value_type);
        }
        stats_.accountedBytes+=sizeof(Lineage)+history.size()*sizeof(const LogRecord*);
        ++stats_.pages;
    }
}

bool ReclaimLineageValidator::provesLater(PageId page,Lsn reclaim,const Page& physical) const {
    const auto found=pages_.find(page);if(found==pages_.end())return false;
    const auto& lineage=found->second;
    const auto lsn=readPersistentPageLsn(physical);
    if(isValidLsn(lsn)) {
        const auto state=lineage.states.find(lsn);
        return lsn>reclaim && state!=lineage.states.end() && after(state->second)==physical;
    }
    const auto bucket=lineage.opaqueByCrc.find(crc32c(physical));
    if(bucket==lineage.opaqueByCrc.end())return false;
    for(auto it=std::upper_bound(bucket->second.begin(),bucket->second.end(),reclaim);it!=bucket->second.end();++it)
        if(after(lineage.states.at(*it))==physical)return true;
    return false; // CRC is an index filter, never the evidence itself.
}
std::optional<DiskManager::Page> ReclaimLineageValidator::afterImage(PageId page,Lsn lsn) const {
    const auto found=pages_.find(page);if(found==pages_.end())return std::nullopt;
    const auto state=found->second.states.find(lsn);
    if(state==found->second.states.end())return std::nullopt;
    return after(state->second);
}
} // namespace minidb
