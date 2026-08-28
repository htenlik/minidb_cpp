# Persistent B+ tree storage format

Milestones 4B.1–4B.2 implement a page-native unique index:

```text
IndexKey (uint32) -> RecordId { PageId, SlotId }
```

All pages are exactly 4096 bytes. Every multi-byte integer is unsigned and encoded
little-endian. No C++ struct, pointer, `std::vector`, or other object representation is
written to disk. The index stores RIDs only and does not read or persist complete Rows.

## Index identity

The caller identifies an index with its stable `IndexMetaPageId`:

```text
Catalog Table Definition -> IndexMetaPageId -> current RootPageId -> tree nodes
```

A root PageId can change when the root splits, while the metadata page does not. Tests
retain `IndexMetaPageId` externally across reopen; Milestone 5B Table Definitions now
own that mapping for primary indexes. The database metadata page's catalog-root field
identifies the Catalog, never an index root. Its free-list-root field
belongs to the global PageAllocator, never to a particular index.

## Index Metadata Page: version 1

Magic: ASCII `MDBIDXMD`. Creation zeroes the complete page.

| Offset | Width | Encoding | Field | Version 1 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | `MDBIDXMD` |
| 8 | 4 | `uint32`, little-endian | Layout version | `1` |
| 12 | 4 | `uint32`, little-endian | Header size | `64` |
| 16 | 4 | `PageId`, little-endian | Root page ID | Root or `INVALID_PAGE_ID` |
| 20 | 8 | `uint64`, little-endian | Entry count | Number of indexed key/RID pairs |
| 28 | 4 | `uint32`, little-endian | Logical leaf maximum keys | `3`–`406` |
| 32 | 4 | `uint32`, little-endian | Logical internal maximum keys | `3`–`507` |
| 36 | 8 | `uint64`, little-endian | Persistent PageLSN | `0` unknown, otherwise global logical LSN |
| 44 | 20 | Zero-filled | Reserved header bytes | Must be zero |
| 64 | 4032 | Zero-filled | Reserved page bytes | Must be zero |

An empty index has `rootPageId = INVALID_PAGE_ID` and `entryCount = 0`; it allocates no
empty root node. A nonempty index must have a legal root PageId and a nonzero count.
Tree size is updated once after a successful unique insertion or erase. Duplicate
insertion and missing-key erase do not alter the count.

## Leaf Page: version 2

Magic: ASCII `MDBIDXLF`. Leaves form a persistent bidirectional chain.

| Offset | Width | Encoding | Field | Version 2 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | `MDBIDXLF` |
| 8 | 4 | `uint32`, little-endian | Node layout version | `2` |
| 12 | 4 | `uint32`, little-endian | Header size | `36` |
| 16 | 4 | `uint32`, little-endian | Key count | Live entries in this leaf |
| 20 | 4 | `PageId`, little-endian | Next leaf | Next leaf or `INVALID_PAGE_ID` |
| 24 | 4 | `PageId`, little-endian | Previous leaf | Previous leaf or `INVALID_PAGE_ID` |
| 28 | 8 | `uint64`, little-endian | Persistent PageLSN | `0` unknown, otherwise global logical LSN |
| 36 | Up to 4060 | Fixed entries | Sorted key/RID entries | 10 bytes each |

Leaf entry `i` begins at `36 + (i * 10)`:

| Entry offset | Width | Encoding | Field |
| ---: | ---: | --- | --- |
| +0 | 4 | `uint32`, little-endian | `IndexKey` |
| +4 | 4 | `PageId`, little-endian | `RecordId.pageId` |
| +8 | 2 | `SlotId`, little-endian | `RecordId.slotId` |

The serialized leaf-entry width is therefore exactly 10 bytes. Stored entries must be
strictly increasing by key and every RID must have a valid non-null representation.
The index does not validate RecordPage occupancy; heap/index coordination belongs to a
higher layer.

Physical leaf capacity is:

```text
floor((4096 - 36) / 10) = 406 entries
36 + (406 * 10) = 4096 bytes used
```

Legacy leaf layout version 1 has a 32-byte header, entries at offset 32, and a four-byte
zero tail. It remains readable with unknown PageLSN and is rewritten as version 2 on a
later mutation. The ten-byte entry width and physical capacity are unchanged.

## Internal Page: version 1

Magic: ASCII `MDBIDXIN`. Internal data uses an interleaved representation:

```text
child0, key0, child1, key1, child2, ...
```

| Offset | Width | Encoding | Field | Version 1 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | `MDBIDXIN` |
| 8 | 4 | `uint32`, little-endian | Node layout version | `1` |
| 12 | 4 | `uint32`, little-endian | Header size | `32` |
| 16 | 4 | `uint32`, little-endian | Key count | Separator count |
| 20 | 4 | `uint32`, little-endian | Child count | Must equal key count + 1 |
| 24 | 8 | `uint64`, little-endian | Persistent PageLSN | `0` unknown, otherwise global logical LSN |
| 32 | 4 | `PageId`, little-endian | Child 0 | Leftmost child |
| 36 | 4 | `uint32`, little-endian | Key 0 | Minimum key in child 1 |
| 40 | 4 | `PageId`, little-endian | Child 1 | Second child |
| ... | ... | ... | Repeated key/child pairs | ... |
| 4092 | 4 | Zero-filled | Maximum-capacity unused tail | Must be zero |

For zero-based indices:

```text
child_offset(i) = 32 + (i * 8)
key_offset(i)   = 36 + (i * 8)
```

The separator convention is identical to Milestone 4A:

```text
keys[i] = smallest key contained in children[i + 1]
children.size() = keys.size() + 1
```

Physical internal capacity and fanout are:

```text
floor((4096 - 32 - 4) / (4-byte key + 4-byte child)) = 507 keys
fanout = 507 + 1 = 508 children
32 + 4 + (507 * 8) = 4092 bytes used
```

## Logical capacities

Physical capacities are fixed by the page layouts. Each index metadata page also stores
logical leaf/internal maximums. Both must be at least 3 and no greater than their
physical maximum. Production-default creation uses `406/507`; tests can persist values
such as `3/3`, `4/4`, or `5/4` to force frequent splits with small datasets. Reopening an
index reloads and validates the same limits.

The validator and deletion algorithm derive non-root occupancy as:

```text
leaf minimum keys         = ceil(logical leaf maximum / 2)
internal minimum children = ceil((logical internal maximum + 1) / 2)
internal minimum keys     = minimum children - 1
```

A leaf root may contain one entry. An internal root must have at least two children;
when only one remains, that child replaces the root. The empty tree has no root page.

## Search, scans, and insertion

Exact lookup uses binary search on internal separators and on the target leaf. It follows
one root-to-leaf PageId path and never scans all index pages.

A range scan descends once to the first relevant leaf, performs a lower-bound search,
and follows persisted `nextLeafPageId` links until the inclusive upper bound is exceeded.
A full scan starts at the leftmost leaf and follows the same chain.

Insertion retains an operation-local path of parent PageIds and child positions. Parent
PageIds are not persisted, avoiding synchronization work and preserving node space.
When a leaf overflows, entries split near the midpoint, a new right page is linked in,
and the old next leaf's backward link is repaired. The right leaf's minimum propagates.

When an internal page overflows, its children split near the midpoint. The separator
equal to the new right subtree's minimum moves to the parent; it is not retained as an
internal key in either child. If propagation passes the old root, a new internal root is
allocated and the metadata page's root PageId is updated. `IndexMetaPageId` does not
change.

Every complete page rewrite zeroes unused bytes through a WritePageGuard, which marks
the frame dirty. Structural insertion writes the old node, new sibling, repaired
neighboring leaf, parents, new root, and index metadata in bounded guard scopes.

## Deletion and rebalancing

`erase(key)` descends with the same operation-local `(parent PageId, child index)` path
used for insertion. A missing key returns false without modifying pages. A successful
erase removes only the key/RID index entry and decrements metadata size exactly once; it
does not erase the referenced RecordStore row.

If a non-root leaf underflows, deletion deterministically borrows from the left sibling,
then the right sibling, when either is above minimum occupancy. Otherwise it merges into
the left sibling when available, or absorbs the right sibling. Merges repair both leaf
links and the adjacent leaf's reverse pointer before reclaiming the eliminated page.

Internal underflow uses the same left-borrow, right-borrow, left-merge, right-merge
policy over child PageIds. Separator arrays are not rotated as B-tree payload keys.
Instead, every affected internal node derives each separator again as:

```text
keys[i] = minimum key reachable through children[i + 1]
```

This also repairs ancestors when deletion changes a subtree minimum without causing an
underflow. Parent underflow repair recurses toward the root. An internal root left with
one child is reclaimed and that child becomes the metadata root. Deleting the last entry
reclaims the leaf root and restores `rootPageId = INVALID_PAGE_ID, entryCount = 0`.

All index metadata and node allocation, including the first leaf and split siblings,
uses the global PageAllocator. Eliminated leaf/internal/root pages are rewritten as Free
Pages and become reusable; see [page-allocation.md](page-allocation.md).

## Reopen and validation

`PersistentBPlusTree::open(bufferPool, diskManager, allocator, indexMetaPageId)` reads
and validates metadata and the root node without reconstructing a whole-tree mirror.
Buffer-backed pages remain the source of truth. Traversal retains PageIds and child
indexes, not guards; each node is decoded to a bounded operation-local value.

The full validator walks PageIds without mutation and checks:

- metadata magic, version, header, zeroed reserved bytes, capacity limits, root, and size;
- no repeated/cyclic node page in the tree graph;
- supported node types/versions and legal PageId references;
- root and non-root occupancy and equal leaf depth;
- strictly ordered leaf entries and internal separators;
- valid RIDs and internal child-count invariants;
- exact right-subtree-minimum separators and disjoint child ranges;
- metadata entry count equals reachable leaf entries; and
- forward/backward leaf chains exactly match tree-order leaves with no cycle, omission,
  or duplicate;
- deletion-era minimum occupancy and root special cases; and
- no metadata or tree-node PageId is also reachable from the global free list.

Corruption is rejected; no automatic repair is attempted.

## Deliberate current limits

The educational fixed RecordStore still uses legacy Pager's append-only allocation
primitive. Catalog stores stable index metadata identities. The B+ tree contains no WAL
codec and has no concurrency control. In the active engine, BufferPoolManager and
RecoveryCoordinator capture guarded split, merge, root, and allocator changes as part
of one implicit WAL-backed statement recovery unit. Direct standalone tree use without
that outer coordinator guarantees persistence only after a successful flush or clean
close and is not crash-atomic.
