#pragma once

#include "minidb/disk_manager.hpp"
#include "minidb/wal_types.hpp"

namespace minidb {

// Operation-time bridge between page mutation and physical WAL. The buffer pool
// remains transaction-agnostic; it only exposes the two durability boundaries.
class PageRecoveryHook {
public:
    virtual ~PageRecoveryHook() = default;
    // Thread-safe lifecycle signal. Idle reader eviction never enters mutable
    // writer state; already-prepared committed frames retain their pageLSN.
    [[nodiscard]] virtual bool needsPreparation() const noexcept { return true; }
    virtual void notePageWriteIntent(PageId pageId, const DiskManager::Page& before) = 0;
    [[nodiscard]] virtual Lsn preparePageForWrite(
        PageId pageId,
        DiskManager::Page& after) = 0;
};

} // namespace minidb
