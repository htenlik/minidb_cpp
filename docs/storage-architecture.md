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

No persistent offset, width, magic, version, tuple encoding, catalog encoding, SQL
grammar, or wire byte changed in 10B. A database assembled through the legacy Pager
formats is opened and queried by the active buffer-backed engine in compatibility tests.

Milestones 11A–11C.2 add sidecar logging/recovery/checkpoint paths without altering a persistent
database-page format:

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

The buffer pool captures write intent and enforces WAL durability before writing a dirty
frame with a valid volatile pageLSN. The production graph attaches LogManager and the
coordinator; TupleStore, tree, Catalog, and Table remain ignorant of WAL encoding.

The engine remains single-threaded. Guards are not locks or latches. 11B adds physical
analysis/REDO/UNDO and one implicit recovery unit per mutating statement. 11C.1 adds a
quiescent full-buffer checkpoint and double-slotted `database.db.ckpt` recovery pointer;
11C.2 splits the global logical WAL into segment files and reclaims whole files behind
the retained checkpoint base. Neither changes database page formats. There is still no
archive/PITR, persistent pageLSN, fuzzy checkpoint, CLR, lock, MVCC, or concurrent
transaction. See [wal.md](wal.md), [wal-segments.md](wal-segments.md),
[recovery.md](recovery.md), and [checkpoints.md](checkpoints.md).
