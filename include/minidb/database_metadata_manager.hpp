#pragma once

#include "minidb/disk_manager.hpp"
#include "minidb/page_recovery.hpp"
#include "minidb/wal_types.hpp"

namespace minidb {

// Focused owner of normal page-0 mutations. DiskManager supplies physical I/O;
// this layer applies the same write-intent/WAL ordering used by buffered pages.
class DatabaseMetadataManager {
public:
    DatabaseMetadataManager(
        DiskManager& diskManager,
        PageRecoveryHook& recoveryHook,
        WalFlushProvider& walProvider)
        : diskManager_(diskManager),
          recoveryHook_(recoveryHook),
          walProvider_(walProvider) {}

    void updateCatalogRootPageId(PageId pageId);
    void updateFreeListRootPageId(PageId pageId);

private:
    DiskManager& diskManager_;
    PageRecoveryHook& recoveryHook_;
    WalFlushProvider& walProvider_;

    void persist(database_format::DatabaseHeader header);
    void validateRoot(PageId pageId, const char* field) const;
};

} // namespace minidb
