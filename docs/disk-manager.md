# DiskManager

Milestone 10A separates physical database-file ownership from page caching. The
database format remains version 1 and every physical page remains 4096 bytes.

```text
active engine                         compatibility path

storage structures                    legacy Pager
        |                                  |
        v                                  |
BufferPoolManager                          |
        |                                  |
        +-------------> DiskManager <------+
                              |
                              v
                         database.db
```

`DiskManager` owns the binary file stream. It initializes or validates page 0 when
opened and exposes these operations:

```cpp
void readPage(PageId, Page& output);
void writePage(PageId, const Page&);
PageId appendPage();
void flush();

PageId pageCount() const;
const database_format::DatabaseHeader& databaseHeader() const;
void updateCatalogRootPageId(PageId);
void updateFreeListRootPageId(PageId);
```

`readPage()` and `writePage()` accept only existing normal data pages. Page 0 is
rejected explicitly; `INVALID_PAGE_ID` and nonexistent IDs are rejected. `appendPage()`
writes one deterministic zero-filled physical page, flushes it, and returns its new ID.
The first append in a new database is page 1. `writePage()` writes and flushes exactly
4096 bytes.

Creation, magic/version/page-size/header-size checks, whole-page file-size validation,
and root metadata persistence moved unchanged from Pager into DiskManager. The exact
metadata encoding remains the layout in [storage-format.md](storage-format.md); no page
type, offset, endianness, or format version changed.

The legacy Pager delegates reads, writes, appends, and metadata updates to one owned
DiskManager while retaining its existing unbounded frame map, raw `Page&` API, dirty
tracking, destructor flush, and `PagerStats` definitions. In 10B, PageAllocator,
TupleStore, PersistentBPlusTree, Catalog, Table, SQL, and TCP instead use the bounded
BufferPoolManager. The legacy fixed RecordStore and Pager-specific tests/benchmarks keep
the old API intentionally.

DiskManager deliberately has no cache, replacement policy, pin count, dirty state,
PageAllocator free-list logic, WAL rule, or thread synchronization. Those belong to
other layers. Physical I/O completion does not imply transactional durability or crash
recovery.
