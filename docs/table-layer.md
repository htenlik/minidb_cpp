# Schema-aware table layer

`Table` composes one immutable Table Definition, one persistent TupleStore, and an
optional persistent B+ tree primary index. It holds no full in-memory row mirror.

```text
RowValues --TupleCodec--> TupleStore
    |                         ^
    +-- UINT32 primary key    | RecordId
              |               |
              +--> Persistent B+ tree
```

## API and mutation semantics

- `insert(values) -> RecordId` validates and encodes the row. A PK table first rejects
  duplicates, inserts the tuple, then inserts `key -> RID`; an unexpected index failure
  triggers best-effort heap rollback. Tables without a PK own no index and allow
  duplicate rows.
- `findByPrimaryKey(key)` performs persistent B+ tree lookup, O(log N), then O(1)
  TupleStore RID lookup plus tuple decoding. It never scans the heap. Calling it on a
  no-PK table is an error.
- `get(RID)` directly fetches and decodes the owning heap tuple.
- `scan()` follows TupleStore page/slot order and decodes each tuple.
- `eraseByPrimaryKey(key)` removes the index entry before erasing the tuple and attempts
  to restore the index mapping if the heap step unexpectedly fails.
- `erase(RID)` is also available and coordinates the primary index when present.
- `update(RID, values)` is the common higher-layer mutation boundary. On indexed tables
  it verifies the RID/key mapping and delegates to coordinated primary-key update logic.
  On no-PK tables it updates in-page or performs compensated cross-page replacement.
- `updateByPrimaryKey(oldKey, values)` validates first and rejects duplicate replacement
  keys without mutation. It preserves the RID when TupleStore can update in-page. When
  the replacement cannot fit that page, Table inserts it elsewhere, installs the new
  key/RID mapping, removes the old mapping, and erases the old tuple. It returns the
  resulting RID, which changes only for relocation. Unexpected clean-operation errors
  receive best-effort compensation.

TupleStore itself still never relocates an update. Cross-page relocation belongs here
because only Table can coordinate indexes. A deleted/reused physical slot retains the
existing stale-RID limitation; higher layers must remove index references before slot
reuse becomes visible.

## Invariants and validation

Diagnostic `Table::validate()` fully scans storage. Every tuple must decode under the
immutable schema. For a PK table, every heap row has exactly one index entry, every
index RID resolves to that heap, the index key equals the decoded row key, key/RID sets
are unique, sizes agree, and both underlying validators pass. A no-PK table must have no
index metadata.

Multiple tables own distinct heap metadata, heap pages, index metadata, and index-node
pages. `Catalog::validate()` diagnoses ownership aliases; no general ownership registry
is maintained during ordinary operations.

## Complexity and durability

- Table insert without PK: TupleStore first-fit O(P * S) worst case plus encoding.
- Table insert with PK: the heap cost plus B+ tree lookup/insertion.
- RID get: O(1) page/slot access plus O(tuple bytes) copy/decode.
- PK find/erase: O(log N) index work plus direct heap access.
- Update: direct page work when in-place; relocation also performs one first-fit insert.
- Scan: heap-page/directory traversal plus O(total encoded bytes) decoding.
- Validation: O(all heap tuples + all index entries), plus underlying storage checks.

Here P is heap-page count and S is a page's slot count. Catalog lookup remains a linear
catalog scan.

There is no transaction manager or WAL. A table mutation can modify TupleStore,
SlottedPage, PageAllocator, and B+ tree pages. Best-effort in-process compensation does
not make those changes crash-atomic. Only successful normal operations followed by a
clean flush/close/reopen are guaranteed in this milestone.
