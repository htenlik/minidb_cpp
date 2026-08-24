# MiniDB++ storage format

This document describes database format version 1. All persistent multi-byte integers
are unsigned and encoded in little-endian byte order. C++ object representations are
never written directly to disk.

## Physical page layout

MiniDB++ database files are a sequence of 4096-byte pages:

```text
Page 0: database metadata / file header (reserved)
Page 1: allocatable storage
Page 2: allocatable storage
...
```

Page 0 is not accessible through DiskManager, Pager, or BufferPoolManager normal
data-page operations. Their physical page count includes this metadata page. A newly
initialized database therefore
has a page count of 1, and its first normal page allocation returns page ID 1.

`PageId` is an unsigned 32-bit integer. `0xFFFFFFFF` is `INVALID_PAGE_ID` and represents
a missing or uninitialized page reference; it can never identify an allocated page.

## Metadata page: format version 1

The version 1 header occupies bytes 0–63 of page 0. The remainder of the metadata page
is reserved. Newly created databases zero every reserved byte.

| Offset | Size | Encoding | Field | Version 1 value |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Magic | ASCII `MINIDB++` |
| 8 | 4 | `uint32`, little-endian | Database format version | `1` |
| 12 | 4 | `uint32`, little-endian | Page size | `4096` |
| 16 | 4 | `uint32`, little-endian | Header size | `64` |
| 20 | 4 | `PageId`, little-endian | Catalog metadata root page | Catalog Metadata or `INVALID_PAGE_ID` |
| 24 | 4 | `PageId`, little-endian | Free-list root page | Free Page head or `INVALID_PAGE_ID` |
| 28 | 36 | Zero-filled | Reserved header fields | `0` |
| 64 | 4032 | Zero-filled | Reserved metadata-page space | `0` |

The catalog root remains `INVALID_PAGE_ID` until the logical Catalog is explicitly
bootstrapped; existing version-1 databases need no migration. Its metadata and table-
definition formats are documented in [catalog.md](catalog.md). The free-list root is the
persisted head of the reusable page allocator introduced in Milestone 4B.2; new
databases initialize it to `INVALID_PAGE_ID`. Its format is documented in
[page-allocation.md](page-allocation.md).

## Creation and validation

Opening a nonexistent or empty file writes and flushes a complete metadata page before
normal pages can be allocated. Opening a nonempty file rejects it when:

- its byte size is not a multiple of 4096;
- its magic is not `MINIDB++`;
- its format version is not the currently supported version 1;
- its stored page size is not 4096; or
- its stored header size is not 64.

There are no format migrations yet. An unsupported version is rejected rather than
being guessed or silently reinterpreted.

Magic bytes distinguish MiniDB++ files from arbitrary data. A version makes future
format changes detectable. Explicit endianness makes integer bytes architecture-
independent. Validation prevents corrupted or incompatible files from being treated as
valid page storage.

## Relationship to row serialization

The database format version describes the file and page-level container. The current
fixed row encoding independently describes a 294-byte row value. Record placement inside
data pages is defined by the RecordPage layout below.

## Record identifiers

A persistent record identifier is:

```text
RecordId {
    PageId pageId;  // uint32
    SlotId slotId;  // uint16
}
```

`INVALID_SLOT_ID` is `0xFFFF`. A valid RID contains neither `INVALID_PAGE_ID` nor
`INVALID_SLOT_ID`, and page 0 can never be a RID page. RIDs contain physical identifiers,
not process-local pointers, so an occupied record remains addressable after the database
closes and reopens. Future indexes can map keys to these RIDs without depending on the
RecordStore implementation.

Updates overwrite the same fixed-size slot. Deletes clear occupancy without moving any
other row, so other RIDs remain stable. A deleted slot can later be reused; without a
generation counter, a stale RID for that deleted slot will then identify the replacement
record. VACUUM and compaction semantics are not defined yet.

## RecordPage layout: version 1

RecordPage layout version 1 is separate from database file format version 1 and the row
encoding. Every persistent multi-byte field is little-endian.

| Offset | Size | Encoding | Field | Version 1 value or meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | Raw bytes | Record-page magic/type | ASCII `MDBRECPG` |
| 8 | 4 | `uint32`, little-endian | RecordPage layout version | `1` |
| 12 | 4 | `uint32`, little-endian | RecordPage header size | `32` |
| 16 | 4 | `PageId`, little-endian | Next record-page ID | Next page or `INVALID_PAGE_ID` |
| 20 | 2 | `uint16`, little-endian | Live-record count | Occupied slots, `0`–`13` |
| 22 | 2 | `uint16`, little-endian | Slot capacity | `13` |
| 24 | 4 | `uint32`, little-endian | Serialized slot size | `294` |
| 28 | 4 | Zero-filled | Reserved header field | `0` |
| 32 | 2 | Bitmap | Slot occupancy | One bit per slot |
| 34 | 3822 | Fixed slots | 13 serialized Rows | 294 bytes per slot |
| 3856 | 240 | Unused | Reserved page tail | Zero on initialization |

Occupancy byte 0 uses bits 0–7 for slots 0–7. Occupancy byte 1 uses bits 0–4 for slots
8–12; its remaining bits must be zero. A set bit means the corresponding slot is live.
Row values and deletion sentinels are never used to infer occupancy.

Slot `i` begins at:

```text
slot_offset(i) = 34 + (i * 294), for 0 <= i < 13
```

The capacity is derived as the largest `N` satisfying:

```text
32 + ceil(N / 8) + (N * 294) <= 4096
```

For `N = 13`, the used size is `32 + 2 + 3822 = 3856` bytes. `N = 14` would require
`32 + 2 + 4116 = 4150` bytes, so it does not fit.

When opening a RecordPage, MiniDB++ validates its magic, supported layout version,
header size, slot capacity, serialized slot size, reserved field, next-page ID, live
count, occupancy count, and unused occupancy bits. Corruption is rejected without repair.

## RecordStore heap chain

A RecordStore is a singly linked sequence of RecordPages. Its head page ID is supplied
explicitly when reopening because this legacy fixed-row heap is not catalog-managed:

```text
head -> RecordPage -> RecordPage -> ... -> INVALID_PAGE_ID
```

Insertion scans the chain for the first free slot. If every page is full, it allocates,
initializes, and links a new RecordPage. Full scans return live `(RecordId, Row)` pairs in
linked-page order and then increasing slot-ID order. RecordStore validates the chain and
rejects dangling links, invalid page types, and cycles.

The fixed-slot region remains the immutable legacy Row format. Variable-length tuples
use a separate SlottedPage page type and TupleStore rather than changing RecordPage v1.
Both layers share the `(pageId, slotId)` RID shape.

## Variable-length tuple heap

SlottedPage and Tuple Heap Metadata pages have independent versioned formats. They store
opaque byte sequences, use stable slot-directory positions as SlotIds, compact tuple
payloads without changing RIDs, and allocate/reclaim pages through PageAllocator. Their
exact byte layouts and validation rules are documented in
[slotted-pages.md](slotted-pages.md).

## Persistent index pages

Persistent B+ tree metadata, leaf, and internal pages use independent versioned page
formats. They do not reuse the database catalog-root placeholder. Their exact layouts,
capacities, links, and validation rules are documented in
[bplus-tree-storage.md](bplus-tree-storage.md).

## Logical schemas, tuples, catalog, and tables

Logical tuple and schema encodings have independent versions and are documented in
[schema-and-tuples.md](schema-and-tuples.md). The Catalog Metadata Page and catalog
Table Definition tuple formats are documented in [catalog.md](catalog.md). Their
composition with TupleStore and the persistent primary index is described in
[table-layer.md](table-layer.md).
