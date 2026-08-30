# Storage architecture

## Active and legacy paths

The legacy implementation used an unbounded process-local Pager cache:

```text
Storage -> legacy Pager -> DiskManager -> database.db
```

That API remains for historical regression coverage; the active relational engine is:

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

Recovery metadata remains close to each frame: PageId, volatile PageLSN, dirty state,
and the earliest recLSN of the current dirty period. Checkpoints obtain a canonical DPT
snapshot through a focused buffer-pool API; SQL, Table, and storage formats do not own a
duplicate DPT.

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
and append allocation. Transactional page-0 root changes pass through the focused
DatabaseMetadataManager and RecoveryCoordinator. The persistent free-list encoding and
LIFO behavior are unchanged. An
allocator release rejects a page if any guard other than its own still pins it.

Catalog bootstrap allocates catalog metadata and its TupleStore through PageAllocator,
writes guarded catalog bytes, and publishes only the catalog metadata PageId in page 0.
The stable heap/index metadata PageIds remain the identities that survive root or data-
page changes.

## Lifecycle and statement boundary

`DatabaseServer` starts owners in dependency order:

```text
DiskManager -> deferred LogManager -> CheckpointControl -> startup RecoveryManager
            -> RecoveryCoordinator -> BufferPoolManager -> CheckpointManager
            -> PageAllocator -> Catalog -> SqlEngine -> TcpServer
```

Reverse destruction removes consumers before storage managers. A mutating SQL result is
returned only after its COMMIT record is fsynced. Database pages are not forced, and
SELECT performs no global flush. Selected tests confirm there are no retained operation
guards at statement boundaries.

## Compatibility and recovery boundary

Database format version 2 adds explicit PageLSN slots to active persistent page formats
and a crash-safe version-1 startup migration. Tuple, catalog-definition, SQL, and wire
encodings are unchanged. Existing non-metadata pages migrate lazily rather than through
a mass rewrite.

```text
RecoveryCoordinator
        |                    |
        | log record         | page bytes + assigned LSN
        v                    v
   LogManager <------ BufferPoolManager ------> DiskManager
        |              force WAL first               |
        v                                             v
 database.db.wal.d/                             database.db
```

The buffer pool captures write intent. The coordinator appends a PageLSN-aware physical
record, writes the returned global LSN to both the persistent page slot and volatile
frame state, and the pool enforces WAL durability before writing the dirty frame. The
production graph attaches LogManager and the
coordinator; TupleStore, tree, Catalog, and Table remain ignorant of WAL encoding.

The engine remains single-threaded. Guards are not locks or latches. Physical
analysis/REDO/UNDO provides one implicit recovery unit per mutating statement. Sharp
full-buffer and opt-in fuzzy DPT checkpoints share the double-slotted `database.db.ckpt`
recovery pointer; segmented WAL reclaims whole files behind the mode-specific floor.
Selective REDO additionally skips a committed v2 update when the disk PageLSN is equal
or newer. There is still no archive/PITR, transaction-overlapping checkpoint,
CLR, lock, MVCC, concurrent transaction, or torn-page protection. See [wal.md](wal.md),
[wal-segments.md](wal-segments.md), [fuzzy-checkpoints.md](fuzzy-checkpoints.md), [recovery.md](recovery.md),
[checkpoints.md](checkpoints.md), and [page-lsn.md](page-lsn.md).
