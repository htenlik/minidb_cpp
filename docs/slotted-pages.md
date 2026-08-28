# Variable-length tuple storage

Milestone 5A adds a new tuple heap without changing the legacy 294-byte Row,
RecordPage v1, or RecordStore formats. A tuple is an opaque `std::vector<std::byte>`;
this layer has no schema, column, or SQL knowledge. Zero-length tuples are rejected so
every live slot owns a nonempty payload range. Overflow pages are not implemented.

All pages are 4096 bytes. Persistent multi-byte integers are unsigned little-endian,
and no C++ object representation is written to disk.

## SlottedPage layout: version 1

Magic: ASCII `MDBSLTPG`.

```text
offset 0
+-------------------------------+
| 48-byte header                |
+-------------------------------+
| tuple payloads ->             |
|                    free space |
|             <- slot directory |
+-------------------------------+
offset 4096
```

| Offset | Width | Encoding | Field | Version 1 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | `MDBSLTPG` |
| 8 | 4 | `uint32`, little-endian | Layout version | `1` |
| 12 | 4 | `uint32`, little-endian | Header size | `48` |
| 16 | 2 | `uint16`, little-endian | Slot count | Persistent directory entries |
| 18 | 2 | `uint16`, little-endian | Live count | Occupied slots |
| 20 | 2 | `uint16`, little-endian | Lower boundary | First byte after compact payloads |
| 22 | 2 | `uint16`, little-endian | Upper boundary | First byte of slot directory |
| 24 | 4 | `PageId`, little-endian | Next heap page | Page ID or `INVALID_PAGE_ID` |
| 28 | 4 | `PageId`, little-endian | Previous heap page | Page ID or `INVALID_PAGE_ID` |
| 32 | 4 | `PageId`, little-endian | Owning heap metadata | Stable `HeapMetaPageId` |
| 36 | 8 | `uint64`, little-endian | Persistent PageLSN | `0` unknown, otherwise global logical LSN |
| 44 | 4 | Zero-filled | Reserved | Must be zero |
| 48 | Variable | Raw bytes | Tuple payload region | Compact, non-overlapping payloads |
| `lower` | Variable | Zero-filled | Contiguous free region | Ends at `upper` |
| `upper` | Variable | Slot entries | Reverse-growing directory | Eight bytes per slot |

The heap-owner field permits O(1) validation that a RID page belongs to a particular
TupleStore without scanning the heap chain.

## Slot directory entry

Slot `i` starts at:

```text
slot_offset(i) = 4096 - ((i + 1) * 8)
```

Thus slot 0 occupies bytes 4088–4095, slot 1 occupies 4080–4087, and the directory grows
toward lower addresses.

| Entry offset | Width | Encoding | Field |
| ---: | ---: | --- | --- |
| +0 | 2 | `uint16`, little-endian | Tuple byte offset |
| +2 | 2 | `uint16`, little-endian | Tuple byte length |
| +4 | 2 | `uint16`, little-endian | Flags: `0` free, `1` live |
| +6 | 2 | Zero-filled | Reserved |

A free slot has canonical all-zero fields and remains in the directory. Deletion never
renumbers later slots. A newly inserted tuple reuses the lowest free SlotId before
growing the directory.

## Capacity and free space

After every mutating operation payload bytes are gap-free between byte 48 and `lower`:

```text
upper     = 4096 - (slotCount * 8)
freeSpace = upper - lower
```

Insertion needs the tuple length plus eight bytes only when no reusable slot exists.
The maximum tuple in an otherwise empty page is:

```text
4096 - 48-byte header - 8-byte slot = 4040 bytes
```

The physical maximum directory contains 506 slots. A tuple exceeding 4040 bytes is
rejected; no truncation or overflow-page fallback occurs.

## Insert, delete, compaction, and update

Insertion appends payload bytes at `lower`, writes a reused or new slot, advances the
payload boundary, and marks the page dirty. Retrieval returns owned bytes, so later
compaction cannot invalidate a caller-held view.

Deletion marks only the target slot free, decrements live count, and immediately
compacts all surviving payloads. Compaction may change stored tuple offsets but never
SlotIds or tuple bytes. It also zeroes the contiguous free region.

Same-size updates overwrite in place. Smaller updates compact and reclaim their excess
space. Larger updates succeed only when the existing payload length plus current free
space can hold the replacement. Capacity is checked and replacement bytes are captured
before mutation; a failed update returns `false`, preserves the RID, and leaves the old
tuple unchanged. Cross-page relocation is deliberately not performed.

## Stable RID and stale-RID boundary

Variable-length tuples use the existing:

```text
RecordId { PageId pageId; SlotId slotId; }
```

Payload compaction changes neither component. A deleted slot can later be reused, so an
old RID may then identify a replacement tuple. Higher layers must remove or update index
entries before allowing such reuse to be logically visible. Generation-aware RIDs and
transactional coordination remain future work.

## Tuple Heap Metadata Page: version 1

The stable identity of a heap is its `HeapMetaPageId`, not its current first data page.
Magic: ASCII `MDBHPMET`. Creation zeroes the complete page.

| Offset | Width | Encoding | Field | Version 1 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | `MDBHPMET` |
| 8 | 4 | `uint32`, little-endian | Layout version | `1` |
| 12 | 4 | `uint32`, little-endian | Header size | `64` |
| 16 | 4 | `PageId`, little-endian | First page | SlottedPage or `INVALID_PAGE_ID` |
| 20 | 4 | `PageId`, little-endian | Last page | SlottedPage or `INVALID_PAGE_ID` |
| 24 | 8 | `uint64`, little-endian | Tuple count | Total live tuples |
| 32 | 8 | `uint64`, little-endian | Persistent PageLSN | `0` unknown, otherwise global logical LSN |
| 40 | 24 | Zero-filled | Reserved header bytes | Must be zero |
| 64 | 4032 | Zero-filled | Reserved page bytes | Must be zero |

An empty heap has invalid first/last PageIds and a zero count, so it allocates no empty
data page.

## TupleStore chain and allocation

SlottedPages form a persistent bidirectional chain. Insertion scans from first to last
and uses the first page whose exact free-space calculation accepts the tuple. If none
fits, PageAllocator provides a page, which is initialized and appended to the chain.
This deliberately simple first-fit policy is O(P) for P heap pages; no in-memory free-
space map is hidden behind the API.

Deleting the final tuple on any page unlinks and releases that page, repairing both
neighbors and metadata endpoints. Deleting the heap's final tuple therefore restores
the empty representation. Reclaimed pages can transition between Free Page, SlottedPage,
and B+ tree node formats through the global allocator. Heap metadata remains allocated
for the lifetime of the TupleStore identity.

Scans follow chain order and then increasing SlotId, returning `(RecordId, TupleBytes)`.
Validation checks metadata, page ownership and type, link inverses, cycles, nonempty
reachable pages, exact counts, slot and payload invariants, and disjointness from the
free list.

## Complexity

- SlottedPage `get`: O(1) metadata lookup plus O(tuple length) copy.
- SlottedPage insert: O(S) to find a reusable slot plus O(tuple length) copy.
- SlottedPage erase/size-changing update/compaction: O(4096 + S), bounded by one page.
- TupleStore `get` and update: O(1) page/slot lookup plus tuple copying or compaction.
- TupleStore insert: O(P * S) worst-case first-fit search with current reusable-slot scan.
- TupleStore erase: O(4096 + F) when reclamation performs the allocator's defensive
  O(F) double-free scan; otherwise one-page bounded work.
- TupleStore scan: O(P + directory slots + returned bytes), conventionally O(P + N)
  when directory density is bounded.

Here P is heap-page count, S is slots per page, N is live tuple count, and F is free-list
length.

## Validation and durability boundary

Validation rejects invalid magic/version/header, count or boundary errors, malformed
free slots, illegal flags, invalid or overlapping payload ranges, payload/directory
overlap, gaps, nonzero reserved/free bytes, illegal owner or neighbor PageIds, broken
chain links, cycles, count disagreement, and live/free PageId overlap. Corruption is not
repaired automatically.

Successful flush and clean close/reopen persistence is supported. SlottedPage and
TupleStore do not encode WAL themselves. In the active engine, guarded mutations of
heap metadata, neighbor links, reclaimed pages, and allocator metadata are intercepted
by BufferPoolManager and RecoveryCoordinator for implicit statement recovery. Direct
standalone TupleStore use without that outer coordinator is not crash-atomic.
