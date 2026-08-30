# Persistent PageLSN and selective REDO

MiniDB++ stores the LSN of the newest physical update represented by each active
database page. Recovery can then avoid rewriting a committed update when the page on
disk already contains that update or a later one.

This is a focused optimization of MiniDB++'s serial, physical REDO/UNDO protocol. It is
informed by the ARIES pageLSN rule, but it does not make MiniDB++ ARIES-compliant:
MiniDB++ now combines PageLSN with sharp or opt-in dirty-page-fuzzy checkpoints and
`recLSN`, but still has no compensation log records and at most one active statement
transaction. See [fuzzy-checkpoints.md](fuzzy-checkpoints.md) and the
[ARIES publication record](https://research.ibm.com/publications/aries-a-transaction-recovery-method-supporting-fine-granularity-locking-and-partial-rollbacks-using-write-ahead-logging).

## LSN representation

`Lsn` is the existing global logical WAL byte position (`uint64_t`). It does not reset
when segments rotate or old segments are reclaimed. In memory, `INVALID_LSN` is
`UINT64_MAX`. Persistent page fields use a different encoding:

```text
encoded 0              = unknown / no PageLSN
encoded 1..UINT64_MAX-1 = the same logical LSN value
encoded UINT64_MAX     = malformed and rejected
```

All fields are eight-byte little-endian integers. Valid WAL records start above zero.
The zero encoding lets legacy pages retain an unknown state without inventing a valid
record identity.

## Database format version 2

New databases use database format version 2. Page 0 stores its PageLSN at bytes 28–35;
bytes 36–63 remain reserved and zero. The active persistent page types use these slots:

| Page type / magic | Page layout version | PageLSN offset | Width |
| --- | ---: | ---: | ---: |
| Database metadata / `MINIDB++` | database v2 | 28 | 8 |
| Free Page / `MDBFREEP` | 1 | 20 | 8 |
| Slotted Page / `MDBSLTPG` | 1 | 36 | 8 |
| Tuple Heap Metadata / `MDBHPMET` | 1 | 32 | 8 |
| Index Metadata / `MDBIDXMD` | 1 | 36 | 8 |
| B+ leaf / `MDBIDXLF` | 2 | 28 | 8 |
| B+ internal / `MDBIDXIN` | 1 | 24 | 8 |
| Catalog Metadata / `MDBCAMET` | 1 | 40 | 8 |

The slots came from previously reserved bytes except for B+ leaf pages. Leaf layout v2
increases the header from 32 to 36 bytes, places PageLSN at 28–35, and begins entries at
36. Its ten-byte entry width and physical capacity remain 406, with no unused tail.
Legacy leaf layout v1 remains readable and reports unknown PageLSN; a later rewrite
uses layout v2. Unknown page magic is deliberately not interpreted as a PageLSN-aware
page, while recognized malformed pages are rejected.

Page-format validators continue checking every reserved byte outside the PageLSN slot.
The PageLSN value itself is validated through one format-dispatching accessor rather
than through raw offsets scattered across recovery code.

## Safe version-1 migration

Database format v1 is accepted for compatibility. Startup performs this sequence before
opening Catalog or normal execution:

1. run ordinary recovery using legacy records and unknown PageLSNs;
2. publish a sharp checkpoint, making all pre-migration page state durable;
3. log the page-0 v1-to-v2 image change as a recovery-aware physical update;
4. make its COMMIT durable;
5. publish a final sharp checkpoint.

Only page 0 is eagerly converted. Existing data pages remain byte-identical and migrate
lazily when future writes serialize them; therefore v1 and v2 page layouts may coexist.
Crashes at every migration/checkpoint publication boundary are restartable: before the
format-update COMMIT, the database remains v1; after it, normal winner REDO completes
the v2 page-0 image. Reopening an already migrated database is idempotent.

Migration is one-way for the database header. Old binaries that only accept format v1
cannot reopen a migrated database. No catalog field is reused and no mass page rewrite
is performed.

## WAL records and assignment

Legacy type IDs retain their meanings:

```text
2 = PAGE_UPDATE             (no beforePageLsn)
8 = PAGE_DELTA_UPDATE       (no beforePageLsn)
9 = PAGE_UPDATE_V2          (PageLSN-aware)
10 = PAGE_DELTA_UPDATE_V2   (PageLSN-aware)
```

Type 9 has a 24-byte payload header followed by both 4096-byte images:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | PageId |
| 4 | 4 | flags; bit 0 is `BEFORE_PAGE_EXISTED` |
| 8 | 4 | page size, 4096 |
| 12 | 2 | payload version, 1 |
| 14 | 2 | header size, 24 |
| 16 | 8 | encoded `beforePageLsn` |
| 24 | 4096 | normalized before-image |
| 4120 | 4096 | normalized after-image |

Its payload is 8216 bytes and its complete WAL record is 8264 bytes. Type 10 uses this
32-byte header followed by the existing canonical range stream:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | PageId |
| 4 | 4 | flags |
| 8 | 4 | page size, 4096 |
| 12 | 4 | range count |
| 16 | 2 | payload version, 1 |
| 18 | 2 | header size, 32 |
| 20 | 4 | reserved, zero |
| 24 | 8 | encoded `beforePageLsn` |
| 32 | variable | canonical range stream |

The PageLSN slot is normalized out of before/after images and byte ranges. It is WAL
metadata, not a user page change; logging it recursively would otherwise make each LSN
assignment appear to require another update record.

For an active PageLSN-aware page, the coordinator captures `beforePageLsn` with the
statement's first before-image. It serializes the logical update, appends the WAL record,
receives LSN `R`, writes `R` into the actual page bytes, and sets the buffer frame's
volatile LSN to the same value. Commit preparation performs this for every touched
resident page, so a NO-FORCE frame contains its eventual persistent PageLSN even before
it reaches disk. WAL is still forced through the frame LSN before any page write.

## Selective REDO and UNDO

For a committed v2 update at record LSN `R`, recovery reads persistent pageLSN `P`:

```text
P is unknown  -> apply REDO and persist R
P < R         -> apply REDO and persist R
P >= R        -> skip the physical page update
```

Legacy type-2/type-8 records always replay because they provide no safe comparison.
`AlwaysRedo` is retained as a diagnostic policy and must produce the same final bytes as
the default `PageLsnSelectiveRedo` policy.

Loser UNDO does not use the skip rule. It restores the original physical bytes and the
explicit `beforePageLsn`; an unknown before value is restored as encoded zero. Thus a
STEAL page from a loser cannot retain a misleading newer PageLSN after rollback.

Recovery validates known persistent LSNs against the logical WAL domain. Values below
the WAL header or at/above the known end are rejected. A value in reclaimed history is
legal because global LSNs do not rebase.

## Checkpoints, limitations, and observability

Checkpoint DPT and PageLSN are complementary filters. During fuzzy recovery, DPT
membership and recLSN eliminate records before a page read; PageLSN then suppresses
writes for surviving records already represented on disk. Sharp checkpoints instead
empty the DPT and move the recovery boundary past their END.

PageLSN does not provide torn-page detection. CRC32C protects WAL records, not database
pages, and an eight-byte database write is not assumed atomic. There is no WAL archive
or point-in-time recovery. The established single-statement crash model still requires
the WAL and checkpoint/segment sidecars.

Runtime metrics count persistent assignments and v1/known-v2 pages. Recovery metrics
count PageLSN checks, unknown values, skipped REDOs, applied checked REDOs, legacy REDO
records, recovery page reads/writes, and skip ratio. The benchmark
`recovery_page_lsn_compare` replays identical history with selective and AlwaysRedo
policies and accepts `--redo-persisted-percent 0|50|100`; see
[benchmarking.md](benchmarking.md).
