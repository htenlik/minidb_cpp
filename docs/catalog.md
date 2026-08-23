# Persistent catalog

Milestone 5B activates database metadata's intended `catalogRootPageId`. Pager exposes
only a narrow validated `updateCatalogRootPageId` operation; page 0 remains unavailable
through normal page APIs.

```text
database page 0
    catalogRootPageId
            |
            v
    Catalog Metadata Page
            |
            v
    catalog-entry TupleStore
            |
            +-- Table Definition
            +-- Table Definition
```

The catalog metadata PageId is the stable catalog identity. All catalog-related pages
are allocated through the global `PageAllocator`.

## Catalog Metadata Page: version 1

Magic: ASCII `MDBCAMET`. The complete 4096-byte page is zeroed on creation. Multi-byte
fields are little-endian.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `MDBCAMET` |
| 8 | 4 | Catalog layout version (`1`) |
| 12 | 4 | Header size (`64`) |
| 16 | 4 | Catalog-entry TupleStore HeapMetaPageId |
| 20 | 4 | Flags/reserved (`0`) |
| 24 | 8 | Next monotonically assigned TableId |
| 32 | 8 | Persisted table count |
| 40 | 24 | Reserved header bytes (`0`) |
| 64 | 4032 | Reserved page bytes (`0`) |

`TableId` is an unsigned 64-bit integer. Zero is `INVALID_TABLE_ID`; assignment starts
at one, increases monotonically, and IDs are never reused in this milestone.

## Table Definition encoding: version 1

One encoded definition is stored as an opaque tuple in the catalog-entry TupleStore.
Magic: ASCII `MDBTBLDF`.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `MDBTBLDF` |
| 8 | 4 | Definition version (`1`) |
| 12 | 4 | Header size (`48`) |
| 16 | 4 | Total encoded size |
| 20 | 4 | Flags/reserved (`0`) |
| 24 | 8 | TableId |
| 32 | 4 | Table TupleStore HeapMetaPageId |
| 36 | 4 | Primary B+ tree IndexMetaPageId, or `INVALID_PAGE_ID` |
| 40 | 2 | Normalized table-name byte length |
| 42 | 2 | Reserved (`0`) |
| 44 | 4 | Encoded schema byte length |
| 48 | variable | Normalized table-name bytes |
| after name | variable | Canonical schema encoding |

The encoded definition must fit the TupleStore 4040-byte inline limit. A schema with a
primary key must have a real index metadata PageId; a schema without one must store
`INVALID_PAGE_ID`. Heap/index roots and pointers are never stored here—only stable
metadata PageIds.

## Bootstrap and APIs

Existing format-version-1 databases legitimately contain
`catalogRootPageId = INVALID_PAGE_ID`. `Catalog::openOrCreate` leaves Pager startup
unchanged and bootstraps only when explicitly requested:

1. allocate Catalog Metadata through PageAllocator;
2. create the catalog-entry TupleStore;
3. initialize next TableId to one and count to zero;
4. encode metadata; and
5. install its PageId in database metadata.

`createTable` normalizes and checks the name before allocating a table TupleStore and,
for a `UINT32 PRIMARY KEY`, a persistent B+ tree. It then appends the definition and
updates catalog counters. `findTable(name)`, `findTable(TableId)`, and `listTables()`
scan the small catalog heap: lookup and listing are O(number of tables). Lists are
sorted by ascending TableId.

Validation checks page formats, entry counts, unique names/IDs, monotonic next TableId,
schema/primary-index agreement, legal heap/index identities, underlying table
invariants, disjoint physical ownership among catalog and tables, and disjointness from
the global free list. DROP TABLE, ALTER TABLE, catalog indexes, and schema evolution are
not implemented.

Bootstrap and table creation modify several independently flushed pages and are not
crash-atomic. Clean flush/close/reopen persistence is supported; WAL/recovery is not.
