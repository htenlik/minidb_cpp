# Persistent page allocation

The database-wide reusable `PageAllocator` uses guarded buffer frames in the active
storage path:

```text
storage -> PageAllocator -> BufferPoolManager -> DiskManager -> database file
```

BufferPoolManager owns caching, pins, dirty tracking, and guarded append allocation;
DiskManager owns fixed-size I/O and protected page-0 metadata. `PageAllocator` owns free-list
interpretation, validation, reclamation, and reuse. It is independent of B+ tree page
types and now serves tuple heaps, persistent indexes, and catalog metadata.

## Free-list head

Database metadata reserves a four-byte `freeListRootPageId` at page-0
offset 24. `INVALID_PAGE_ID` means the list is empty. DiskManager exposes a narrow
`updateFreeListRootPageId` operation that validates the new reference and explicitly
serializes and flushes page 0; normal `getPage(0)`, dirty, and flush operations remain
forbidden.

The free list is a persistent singly linked LIFO stack:

```text
page 0 freeListRoot -> Free Page -> Free Page -> INVALID_PAGE_ID
```

## Free Page: layout version 1

Every multi-byte field is unsigned little-endian. Reclamation zeroes the complete page
before writing this header.

| Offset | Width | Encoding | Field | Version 1 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | ASCII `MDBFREEP` |
| 8 | 4 | `uint32`, little-endian | Layout version | `1` |
| 12 | 4 | `uint32`, little-endian | Header size | `32` |
| 16 | 4 | `PageId`, little-endian | Next free page | Page ID or `INVALID_PAGE_ID` |
| 20 | 8 | `uint64`, little-endian | Persistent PageLSN | `0` unknown, otherwise global logical LSN |
| 28 | 4 | Zero-filled | Reserved header bytes | Must be zero |
| 32 | 4064 | Zero-filled | Cleared payload/reserved bytes | Must be zero |

A Free Page cannot simultaneously validate as an index node, RecordPage, or index
metadata page because it has its own magic/type and its former payload is erased.

## Allocation and reclamation

`allocatePage()` pops and validates the free-list head when one exists, updates the
persisted head, clears the selected page, marks it dirty, and returns its PageId for the
caller to initialize. When the list is empty it delegates to the buffer pool's guarded
append primitive. Each free-list page is read under one operation-scoped guard.
The allocator validates the complete list at construction; a normal pop is then O(1).

`releasePage(pageId)` rejects page 0, `INVALID_PAGE_ID`, nonexistent pages, and any page
already reachable from the free list. After acquiring its write guard it also requires
that its own pin is the only pin. It rewrites the page as a Free Page whose `next`
is the old head, marks it dirty, and persists the new head. The defensive duplicate
check traverses the list, so release is O(F) for F free pages. This explicit correctness
tradeoff can be revisited if allocation metadata later provides constant-time ownership
state.

Physical files are never truncated. Reclamation means PageId reuse. Index metadata
pages remain owned for the lifetime of their index; ordinary tree deletion reclaims
only eliminated leaf/internal/root nodes.

## Validation and durability boundary

Validation rejects an illegal root, page 0, out-of-range references, invalid magic,
unsupported version, wrong header size, nonzero reserved bytes, illegal next links,
cycles, and duplicate visits. Persistent B+ tree validation additionally checks that
its metadata and reachable node PageIds are disjoint from the free-list set.

The free list and tree survive successful explicit buffer/DiskManager flushes and clean
close/reopen cycles. PageAllocator does not encode WAL itself. In the active engine,
DatabaseMetadataManager, BufferPoolManager, and RecoveryCoordinator include allocator
and tree changes in the surrounding implicit statement recovery unit. Direct standalone
allocator/tree use without that outer coordinator is not crash-atomic.
