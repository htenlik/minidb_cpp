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

Page 0 is not accessible through the Pager's normal data-page operations. The Pager's
physical page count includes this metadata page. A newly initialized database therefore
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
| 20 | 4 | `PageId`, little-endian | Catalog root page | `INVALID_PAGE_ID` initially |
| 24 | 4 | `PageId`, little-endian | Free-list root page | `INVALID_PAGE_ID` initially |
| 28 | 36 | Zero-filled | Reserved header fields | `0` |
| 64 | 4032 | Zero-filled | Reserved metadata-page space | `0` |

The root fields are placeholders for later storage milestones. Version 1 defines their
location and initial null value but does not yet implement a catalog or free list.

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
data pages is intentionally undefined until the record-storage milestone.
