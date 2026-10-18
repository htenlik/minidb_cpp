#pragma once
#include "minidb/page_reclaim.hpp"
#include <map>
#include <memory>
#include <optional>

namespace minidb {
struct ReclaimLineageStats {
    std::uint64_t pages = 0, transitions = 0, clrs = 0, opaque = 0, accountedBytes = 0;
};

// Recovery-only evidence, built AFTER transaction/CLR chain validation. The
// scan owns records and must outlive this index. No physical database writes.
class ReclaimLineageValidator {
public:
    explicit ReclaimLineageValidator(const std::vector<LogRecord>& records);
    [[nodiscard]] bool provesLater(PageId page, Lsn reclaim, const DiskManager::Page& physical) const;
    [[nodiscard]] std::optional<DiskManager::Page> afterImage(PageId page,Lsn lsn) const;
    [[nodiscard]] const ReclaimLineageStats& stats() const noexcept { return stats_; }
private:
    struct Transition {
        const LogRecord* record;
        // Existing update WAL is cumulative from the transaction's original
        // before-image, not a delta from the immediately preceding record.
        std::shared_ptr<const DiskManager::Page> transactionBase;
    };
    struct Lineage {
        std::map<Lsn, Transition> states;
        std::map<std::uint32_t, std::vector<Lsn>> opaqueByCrc;
    };
    std::map<PageId, Lineage> pages_;
    ReclaimLineageStats stats_;
    [[nodiscard]] static DiskManager::Page after(const Transition& transition);
};
} // namespace minidb
