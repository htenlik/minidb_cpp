# Persistent B+ tree storage format

Milestone 4B.1 implements a page-native unique index:

```text
IndexKey (uint32) -> RecordId { PageId, SlotId }
```

All pages are exactly 4096 bytes. Every multi-byte integer is unsigned and encoded
little-endian. No C++ struct, pointer, `std::vector`, or other object representation is
written to disk. The index stores RIDs only and does not read or persist complete Rows.

## Index identity

The caller identifies an index with its stable `IndexMetaPageId`:

```text
Future catalog -> IndexMetaPageId -> current RootPageId -> tree nodes
```

A root PageId can change when the root splits, while the metadata page does not. Tests
retain `IndexMetaPageId` externally across reopen; a future catalog will own that mapping.
The database metadata page's catalog-root and free-list-root fields are not reused.

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
| 36 | 28 | Zero-filled | Reserved header bytes | Must be zero |
| 64 | 4032 | Zero-filled | Reserved page bytes | Must be zero |

An empty index has `rootPageId = INVALID_PAGE_ID` and `entryCount = 0`; it allocates no
empty root node. A nonempty index must have a legal root PageId and a nonzero count.
Tree size is updated once after a successful unique insertion. Duplicate insertion does
not alter the count.

## Leaf Page: version 1

Magic: ASCII `MDBIDXLF`. Leaves form a persistent bidirectional chain.

| Offset | Width | Encoding | Field | Version 1 meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic/type | `MDBIDXLF` |
| 8 | 4 | `uint32`, little-endian | Node layout version | `1` |
| 12 | 4 | `uint32`, little-endian | Header size | `32` |
| 16 | 4 | `uint32`, little-endian | Key count | Live entries in this leaf |
| 20 | 4 | `PageId`, little-endian | Next leaf | Next leaf or `INVALID_PAGE_ID` |
| 24 | 4 | `PageId`, little-endian | Previous leaf | Previous leaf or `INVALID_PAGE_ID` |
| 28 | 4 | Zero-filled | Reserved | Must be zero |
| 32 | Up to 4060 | Fixed entries | Sorted key/RID entries | 10 bytes each |
| 4092 | 4 | Zero-filled | Maximum-capacity unused tail | Must be zero |

Leaf entry `i` begins at `32 + (i * 10)`:

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
floor((4096 - 32) / 10) = 406 entries
32 + (406 * 10) = 4092 bytes used
```

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
| 24 | 8 | Zero-filled | Reserved | Must be zero |
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

The validator derives non-root insertion occupancy as:

```text
leaf minimum keys     = ceil(logical leaf maximum / 2)
internal minimum keys = floor(logical internal maximum / 2)
```

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

Every complete page rewrite zeroes unused bytes and calls `Pager::markDirty`. Structural
insertion explicitly dirties the old node, new sibling, repaired neighboring leaf,
parents, new root, and index metadata whenever each is modified.

## Reopen and validation

`PersistentBPlusTree::open(pager, indexMetaPageId)` reads and validates metadata and the
root node without reconstructing a whole-tree mirror. Pager pages remain the source of
truth. Normal lookup/insertion decodes only its traversal path and split participants.

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
  or duplicate.

Corruption is rejected; no automatic repair is attempted.

## Deliberate Milestone 4B.1 limits

Persistent deletion, redistribution, merging, root shrinking, node reclamation, free-list
management, catalog integration, concurrency, transactions, WAL, and recovery are not
implemented. Supported operations do not make tree pages unreachable.

Persistence is guaranteed only after a successful flush or clean Pager close. A split
can dirty several related pages, and there is no WAL or atomic multi-page commit. A crash
between physical writes can therefore leave an inconsistent tree. Write ordering alone
is not claimed as crash consistency; recovery belongs to a later transaction/WAL design.
