#include "minidb/database_metadata_manager.hpp"
#include "minidb/recovery.hpp"

#include <stdexcept>
#include <string>

namespace minidb {

void DatabaseMetadataManager::validateRoot(PageId pageId, const char* field) const {
    if (pageId != INVALID_PAGE_ID
        && (pageId == database_format::METADATA_PAGE_ID
            || pageId >= diskManager_.pageCount())) {
        throw std::invalid_argument(std::string(field) + " must identify an existing data page");
    }
}

void DatabaseMetadataManager::persist(database_format::DatabaseHeader header) {
    DiskManager::Page before{};
    diskManager_.readPhysicalPage(database_format::METADATA_PAGE_ID, before);
    recoveryHook_.notePageWriteIntent(database_format::METADATA_PAGE_ID, before);
    DiskManager::Page after{};
    database_format::serializeDatabaseHeader(header, after);
    const auto lsn = recoveryHook_.preparePageForWrite(
        database_format::METADATA_PAGE_ID, after);
    if (isValidLsn(lsn)) walProvider_.flushUpTo(lsn);
    diskManager_.writePhysicalPage(database_format::METADATA_PAGE_ID, after);
    recoveryFailPoint("after_database_page_write");
}

void DatabaseMetadataManager::updateCatalogRootPageId(PageId pageId) {
    validateRoot(pageId, "Catalog root");
    auto header = diskManager_.databaseHeader();
    header.catalogRootPageId = pageId;
    persist(header);
}

void DatabaseMetadataManager::updateFreeListRootPageId(PageId pageId) {
    validateRoot(pageId, "Free-list root");
    auto header = diskManager_.databaseHeader();
    header.freeListRootPageId = pageId;
    persist(header);
}

} // namespace minidb
