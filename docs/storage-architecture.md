# Storage architecture

## Active and legacy paths

The frozen `v0.1.0` implementation used an unbounded process-local Pager cache:

```text
Storage -> legacy Pager -> DiskManager -> database.db
```

Milestone 10B keeps that API for historical regression coverage while changing the
active relational engine to:

```text
TCP Server
    -> SqlEngine
    -> Catalog / Table
    -> PersistentBPlusTree + TupleStore
    -> SlottedPage views / PageAllocator
    -> ReadPageGuard or WritePageGuard
    -> BufferPoolManager
    -> LRU-K replacement
    -> DiskManager
    -> database.db
```

Legacy Pager still backs the fixed-size educational RecordPage/RecordStore, the small
page CLI, Pager regression tests, and benchmarks labeled `legacy_pager`. It is not used
by production TupleStore, PersistentBPlusTree, PageAllocator, Catalog, Table, SQL, or TCP.

## Ownership and page lifetime

DiskManager owns the file. BufferPoolManager owns a fixed vector of volatile frames.
Page guards own pins into those frames. Persistent storage objects own only references
to their longer-lived managers plus PageIds such as `HeapMetaPageId` or
`IndexMetaPageId`.

The hard lifetime rule is:

> Frame bytes may be accessed only while the guard that pins that page is alive.

Page-format code is non-owning. `ConstSlottedPageView` interprets immutable guarded
bytes and `SlottedPageView` mutates writable guarded bytes. Persistent B+ tree pages are
decoded to operation-local, one-page-sized logical values before their guards leave
scope. Such views and spans are never stored in TupleStore, tree, Catalog, or Table
objects.

`ReadPageGuard` is used for interpretation and validation. `WritePageGuard` is used for
mutation and provides dirty tracking. When all frames are pinned, helper acquisition
functions raise `StorageError(NoFrameAvailable)`; production code never falls back to
the legacy Pager.

## Multi-page operations

Operations are decomposed into phases and carry stable PageIds between them:

- TupleStore first-fit insertion examines one page at a time, drops the read guard,
  reacquires one write guard, and rechecks capacity.
- Heap append/unlink copies previous/next IDs, then repairs each page and metadata in
  separate guard scopes. Reclaimed pages are released only after the page guard drops.
- B+ tree traversal paths contain `{pageId, childIndex}`, never root-to-leaf guards.
- Tree splits, borrow, merge, neighbor-link repair, and root changes use bounded logical
  copies and write each affected PageId in a separate scope.
- Validators decode local state, drop the guard, and recurse or advance using copied
  PageIds. Visited sets and summaries are ordinary non-page memory.

This currently keeps the intended maximum at one pinned frame for high-level operations.
The engine is tested end-to-end with three frames and the directed workflow also passes
with two. See [buffer-pool.md](buffer-pool.md) for the detailed pin budget.

## Allocation and metadata

PageAllocator uses the BufferPoolManager for free-page validation, free-page rewrites,
and append allocation, while page-0 root updates remain explicit DiskManager metadata
operations. The persistent free-list encoding and LIFO behavior are unchanged. An
allocator release rejects a page if any guard other than its own still pins it.

Catalog bootstrap allocates catalog metadata and its TupleStore through PageAllocator,
writes guarded catalog bytes, and publishes only the catalog metadata PageId in page 0.
The stable heap/index metadata PageIds remain the identities that survive root or data-
page changes.

## Lifecycle and statement boundary

`DatabaseServer` declares owners in dependency order:

```text
DiskManager -> BufferPoolManager -> PageAllocator -> Catalog -> SqlEngine -> TcpServer
```

Reverse destruction removes network/execution consumers before storage managers and
destroys/flushes the buffer pool before DiskManager. On a successful TCP statement the
result is materialized, the buffer pool flushes all dirty frames, DiskManager is flushed,
and only then is the success response sent. Selected tests confirm there are no retained
operation guards at request boundaries.

## Compatibility and recovery boundary

No persistent offset, width, magic, version, tuple encoding, catalog encoding, SQL
grammar, or wire byte changed in 10B. A database assembled through the legacy Pager
formats is opened and queried by the active buffer-backed engine in compatibility tests.

The engine remains single-threaded. Read/write guards express access mode, pin lifetime,
and dirty permission—not locks or latches. There is no WAL, transaction atomicity,
recovery, `fsync` guarantee, or synchronization protocol. Dirty eviction can persist
pages in an order unrelated to a logical multi-page operation. A future WAL must enforce
write-ahead ordering before dirty transactional pages may be written.
