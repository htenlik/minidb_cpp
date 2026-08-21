# In-memory B+ tree primary index

Milestone 4A implements one concrete, unique-key index:

```text
IndexKey (uint32) -> RecordId { PageId, SlotId }
```

The index stores RIDs rather than complete Rows and does not modify `RecordStore`.
Callers remain responsible for coordinating heap and index changes. The tree is entirely
in memory; closing the process discards it.

## B+ tree structure

Unlike a B-tree, actual key/RID entries exist only in leaves. Internal keys route a
search but do not remove promoted keys from the leaf level. Leaves are linked in both
directions, allowing a range scan to descend once and then walk consecutive leaves.

MiniDB++ uses this separator convention:

```text
internal.keys[i] = smallest key in internal.children[i + 1]
internal.children.size() == internal.keys.size() + 1

                    [ 20 | 50 ]
                   /     |      \
              [< 20]  [20..49]  [>= 50]
```

Search uses the first separator strictly greater than the requested key. Separators are
derived from child subtree minima after every operation that can change a minimum,
including deletion and redistribution—not only split and merge operations.

Nodes are owned recursively by `std::unique_ptr`: the tree owns its root and internal
nodes own their children. Parent pointers and the leaf `previous`/`next` links are
non-owning pointers. Structural operations update those pointers before relinquishing
or transferring ownership.

## Capacity and occupancy

Leaf and internal maximum key counts are independently configurable. Both must be at
least 3; the default is 64 for each. Small values deliberately make tests exercise
balancing frequently.

For configured maxima `L` (leaf) and `I` (internal):

```text
non-root leaf minimum keys     = ceil(L / 2)
non-root internal minimum keys = floor(I / 2)
non-root internal children     = keys + 1
```

The root is special. An empty tree has no root. A leaf root contains from 1 through `L`
entries. An internal root contains at least one separator and two children. If deletion
leaves an internal root with one child, that child becomes the new root.

## Insertion and splitting

Insertion descends through internal separators with binary search, then uses binary
search within the target leaf. Duplicate keys return `false`; the existing RID is not
overwritten. Invalid RIDs are rejected.

An overflowing leaf contains `L + 1` entries. It retains the first
`ceil((L + 1) / 2)` entries and moves the remainder to a new right leaf. Both leaf links
are repaired. The right leaf's minimum becomes the parent separator, while that key and
RID remain in the leaf.

An overflowing internal node contains `I + 2` children. Its children are divided around
the midpoint, parent pointers are repaired, and separators on both halves are rebuilt
from subtree minima. A new sibling is inserted recursively into the parent. Splitting
the root creates a new internal root and increases the height by one.

## Deletion and rebalancing

Erasing a missing key returns `false`. Removing an existing entry first updates its leaf.
If a non-root node falls below minimum occupancy, rebalancing proceeds as follows:

1. Borrow one entry or child from a left sibling that is above its minimum.
2. Otherwise borrow from an eligible right sibling.
3. Otherwise merge with the left sibling, or with the right sibling when no left sibling
   exists.
4. Remove the redundant child from the parent and recursively rebalance an underfull
   internal parent.

Leaf merges also repair both directions of the leaf chain. Internal redistribution and
merging transfer child ownership and reset every moved child's parent pointer. Root
shrink is repeated naturally as underflow propagates; deleting the final leaf entry
returns the tree to its initial empty state.

## Lookup and range scans

- `find(key)` descends one root-to-leaf path: `O(log N)` for bounded node capacity.
- `insert(key, rid)` and `erase(key)` touch one path plus bounded sibling work:
  `O(log N)` structural work.
- `rangeScan(lower, upper)` descends to `lower`, then follows leaf links:
  `O(log N + K)` for `K` results.
- `scanAll()` walks the forward leaf chain once: `O(N)`.

Range bounds are inclusive. A reversed range (`lower > upper`) and every range on an
empty tree return no entries. Bounds need not exist in the tree, and the full uint32 key
domain—including 0 and `UINT32_MAX`—is supported.

Vectors make searches within nodes logarithmic but insertion/erasure within one node
linear in that node's configured capacity. With a fixed page-oriented capacity, this is
bounded local work and does not become a scan of the indexed dataset.

## Validation

`validate()` is non-mutating and checks:

- stored size equals the number of leaf entries;
- every node's keys are strictly sorted;
- all leaves occur at the same depth and contain equal key/value counts;
- root and non-root occupancy rules;
- internal child count, child ownership, and parent-pointer consistency;
- non-overlapping child key ranges and exact right-subtree-minimum separators;
- valid RIDs in every leaf;
- depth-first leaf order agrees with forward and backward links;
- adjacent leaf ranges are strictly ordered; and
- leaf links contain no missing, duplicated, extra, or cyclic nodes.

Tests invoke validation after deterministic, adversarial, and randomized mutations.

## Heap coordination and stale RIDs

A deleted heap slot can be reused, and the current RID has no generation component.
Therefore the current higher-layer invariant is:

```text
An index referencing a heap record must remove or update its index entry before that
heap slot may be logically considered reusable by higher layers.
```

Transactions, WAL, crash atomicity, and generation-aware RIDs remain future design work.
Milestone 4A intentionally does not redesign the persistent RID or RecordPage format.

## Milestone 4B persistence boundary

The separator convention, search decisions, occupancy mathematics, split/borrow/merge
rules, root cases, and leaf-chain range algorithm can guide the persistent tree.
The current node representation cannot be persisted directly. Milestone 4B must replace
pointer ownership with page IDs and page pin/access lifetimes, define explicit node byte
layouts and versions, mark modified pages dirty, validate persistent node fields, and
connect the metadata/catalog root through a separately reviewed design. Raw pointers,
`std::vector`, and whole-object dumps are not an on-disk format.
