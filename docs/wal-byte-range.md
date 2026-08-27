# Physical byte-range WAL experiment

MiniDB++ supports two physical page-update encodings and three generation modes.
`FullPage` is the production
default and retains the established `PAGE_UPDATE` record with complete 4096-byte
before/after images. The opt-in `ByteRange` mode writes `PAGE_DELTA_UPDATE` records
containing only transaction-touched byte ranges. Both encodings participate in the same
statement transaction chain, STEAL / NO-FORCE protocol, sharp checkpoints, segmented
WAL, and REDO/UNDO implementation. `Adaptive` chooses the smaller complete encoding per
record as documented in [wal-adaptive.md](wal-adaptive.md).

This is a controlled logging-format experiment, not a claim that byte ranges always
win. A scattered update can create many range descriptors, and an existing-page delta
whose alternating bytes changed is larger than the full-page baseline. Delta discovery
also adds a linear page comparison to the write path.

## Record identity and payload

`PAGE_DELTA_UPDATE` has stable WAL record type ID `8`. The ordinary 48-byte WAL record
header and CRC32C cover the following version-1 payload. Every integer is little-endian;
no C++ object representation is persisted.

| Offset | Width | Field | Version-1 invariant |
| ---: | ---: | --- | --- |
| 0 | 4 | `PageId` | not `INVALID_PAGE_ID` |
| 4 | 4 | flags | bit 0 is `BEFORE_PAGE_EXISTED`; all other bits zero |
| 8 | 4 | page size | exactly `4096` |
| 12 | 4 | range count | `1..2048` |
| 16 | 2 | payload version | `1` |
| 18 | 2 | fixed header size | `24` |
| 20 | 4 | reserved | zero |
| 24 | variable | range stream | exact encoding below |

Each range begins with this four-byte descriptor:

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | byte offset in the 4096-byte page |
| 2 | 2 | nonzero byte length |
| 4 | `length` | original before bytes, existing pages only |
| `4 + length` | `length` | after bytes, existing pages |
| 4 | `length` | after bytes, newly appended pages |

For an existing page, payload bytes are therefore
`24 + sum(4 + 2 * length)`. For a new page they are
`24 + sum(4 + length)`: the omitted before bytes are logically zero and recovery
reconstructs that zero image when decoding. A complete WAL record adds the ordinary
48-byte record header.

The decoder bounds the range count before allocation and rejects invalid IDs, unknown
flags, wrong page/version/header values, nonzero reserved fields, zero-length or
out-of-page ranges, overflow, overlap, duplicates, noncanonical ordering/adjacency,
truncated before/after data, inconsistent lengths, and trailing bytes. CRC32C protects
the complete outer record.

## Canonical delta computation

`computePageDelta(before, after)` makes one deterministic `O(PAGE_SIZE)` pass. It finds
maximal consecutive changed runs, emits them in ascending offset order, and therefore
merges adjacent changed bytes. Exact lookup within the WAL remains page-oriented;
TupleStore, B+ tree, catalog, and SQL code do not know which WAL encoding is selected.

No update record is generated when the current page is byte-identical to the last
logged state. With repeated writes and STEAL, the coordinator maintains a volatile
cumulative touched-byte mask for each page. Every later delta covers all bytes touched
by the transaction, uses the original statement-start bytes as `before`, and uses the
current state as `after`. A byte changed in an earlier stolen image and later restored
to its original value remains in the coverage (its before and after bytes are equal).
Consequently, the latest delta alone can restore the complete pre-statement page while
ordered REDO still reproduces intermediate reversions exactly. The mask and full images
are transient; no database-page format changed.

## REDO, UNDO, and page existence

Winner REDO reads the current 4096-byte page (or starts from zero for an appended page),
copies every range's after bytes, and writes the resulting physical page. Loser UNDO
walks update records backward; for the first/latest record for each existing page it
copies original before bytes. Newly appended loser pages are removed by truncating to
the page count recorded in `BEGIN`, as in full-page mode.

Before any dirty STEAL write, the coordinator appends the selected update encoding and
the buffer pool forces WAL through the returned volatile page LSN. Commit remains
NO-FORCE: only WAL through `COMMIT` is forced, so committed database pages may require
REDO after restart.

WAL history can freely contain full-page, byte-range, and later full-page transactions.
Recovery dispatches by each record's type ID; the currently selected generation mode
does not reinterpret retained history. The sharp-checkpoint control format, segment
format, global LSN mapping, reclamation rules, database pages, and wire protocol are
unchanged.

## Configuration

The server and transaction/recovery benchmarks accept:

```text
--wal-update-mode full-page   # default
--wal-update-mode byte-range  # opt-in experiment
--wal-update-mode adaptive    # opt-in per-record minimum-size selection
```

Invalid values are rejected. Selection affects newly generated page-update records
only. Existing WAL is always decoded from its stable record type.

## Metrics and comparison method

Transaction metrics use observed page bytes rather than a tuple-size estimate:

- `logicalBytesChanged`: byte transitions between the last logged state and the next
  state submitted for persistence;
- `walUpdatePayloadBytes`: update payload bytes excluding the 48-byte outer headers;
- `walTotalBytesGenerated`: complete BEGIN/update/COMMIT/ABORT bytes generated by the
  transaction coordinator;
- `rangeCount`: ranges across delta update records;
- `changedBytes`: bytes represented by update records (4096 per full-page record, range
  lengths per delta record);
- `updateRecordCount`, per-mode record counts, record-size samples, and
  `deltaComputationNs`; Adaptive adds candidate-byte, selection-count/tie, saved-byte,
  and selection-time counters described in [wal-adaptive.md](wal-adaptive.md).

The benchmark JSON reports mean/p50/p95/p99/max update-record bytes and
mean/p50/p95/max ranges per delta. It derives payload amplification as
`walUpdatePayloadBytes / logicalBytesChanged` and total amplification as all LogManager
bytes appended during the measured workload divided by `logicalBytesChanged`; a zero
denominator produces `0`, not infinity. Automatic checkpoint records are included in
the total but not update-payload amplification.

Controlled comparisons must keep the Release build, machine, seed, row/operation count,
buffer frames, LRU-K value, segment capacity, and checkpoint policy identical, changing
only `--wal-update-mode`. Suggested workload pairs are `txn_update` (small scalar),
`txn_varchar_update`, `txn_insert`, `txn_delete`, `txn_bplus_insert` near a physical
split boundary, and `txn_mixed`. `recovery_full_scan` and `recovery_loser` expose winner
REDO and loser UNDO/truncation timing respectively.

## Controlled measurement snapshot

The following single-run snapshot was captured on 2026-08-27 on an Apple M2 running
macOS 26.5.2, using AppleClang 21, one warning-clean Release binary, seed 12345,
64 buffer frames, LRU-K `K=2`, 16-MiB WAL segments, and automatic checkpoints disabled.
The only paired setting changed was the WAL update mode. Setup rows were 200 for scalar,
VARCHAR, and delete; 300 for mixed; zero for insert; and 406 for the 16-operation B+
boundary workload. This is experimental evidence on one machine, not a universal
performance claim.

| Workload | Mode | ops/s | p95 us | p99 us | WAL bytes | fsyncs | updates | total amp. |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| scalar update | FullPage | 4,932 | 288.1 | 487.8 | 1,673,600 | 200 | 200 | 4,184.00 |
| scalar update | ByteRange | 13,807 | 81.6 | 94.9 | 38,400 | 200 | 200 | 96.00 |
| VARCHAR update | FullPage | 4,668 | 314.0 | 499.0 | 1,896,512 | 200 | 227 | 7.85 |
| VARCHAR update | ByteRange | 6,742 | 242.1 | 372.7 | 785,228 | 200 | 227 | 3.25 |
| INSERT | FullPage | 3,434 | 299.9 | 557.3 | 6,643,712 | 200 | 802 | 873.71 |
| INSERT | ByteRange | 10,823 | 110.5 | 122.9 | 111,942 | 200 | 802 | 14.72 |
| DELETE | FullPage | 3,258 | 308.7 | 834.6 | 6,676,736 | 204 | 806 | 52.91 |
| DELETE | ByteRange | 6,432 | 279.4 | 352.7 | 619,186 | 204 | 806 | 4.91 |
| B+ boundary insert | FullPage | 3,308 | 436.6 | 436.6 | 546,688 | 16 | 66 | 248.38 |
| B+ boundary insert | ByteRange | 9,036 | 270.5 | 270.5 | 16,367 | 16 | 66 | 7.44 |
| mixed SQL | FullPage | 3,633 | 308.5 | 542.7 | 7,464,000 | 300 | 900 | 48.07 |
| mixed SQL | ByteRange | 7,280 | 306.6 | 379.3 | 724,326 | 300 | 900 | 4.66 |

Relative ByteRange differences in this snapshot were: scalar update `+179.9%` ops/s and
`-97.7%` WAL bytes; VARCHAR update `+44.4%` and `-58.6%`; INSERT `+215.2%` and
`-98.3%`; DELETE `+97.4%` and `-90.7%`; B+ boundary insert `+173.2%` and `-97.0%`;
mixed SQL `+100.4%` and `-90.3%`. p95 changed by `-71.7%`, `-22.9%`, `-63.1%`,
`-9.5%`, `-38.0%`, and `-0.6%` respectively. The unchanged fsync counts confirm that
this comparison changed representation, not commit durability frequency.

FullPage update records were always 8,256 bytes. ByteRange exposed the expected
fragmentation tradeoff:

| Workload | record bytes mean / p95 / p99 / max | ranges mean / p95 / max | diff ns/update |
| --- | ---: | ---: | ---: |
| scalar update | 80 / 80 / 80 / 80 | 1.0 / 1 / 1 | 2,604 |
| VARCHAR update | 3,360 / 6,954 / 7,260 / 7,362 | 290.1 / 603 / 645 | 12,775 |
| INSERT | 112 / 196 / 196 / 198 | 5.2 / 15 / 19 | 2,819 |
| DELETE | 740 / 2,472 / 5,888 / 6,050 | 88.8 / 370 / 479 | 5,649 |
| B+ boundary insert | 221 / 200 / 4,008 / 4,008 | 23.5 / 15 / 610 | 3,570 |
| mixed SQL | 767 / 4,022 / 6,684 / 8,274 | 87.6 / 513 / 911 | 5,589 |

The mixed maximum (8,274 bytes) exceeded a full-page record (8,256 bytes), confirming
that highly scattered deltas are not guaranteed to be smaller.

One otherwise identical 400-operation mixed run used 16-KiB segments and a sharp
checkpoint every 50 committed statements. Both modes completed eight checkpoints.
FullPage appended 9,936,896 WAL bytes, rotated 1,198 times, reclaimed 10,038,528 bytes,
and retained 25,360 physical bytes; ByteRange appended 1,083,040 bytes, rotated 80
times, reclaimed 1,095,738 bytes, and retained 21,796 bytes. This changes how often the
existing segment/checkpoint thresholds are reached; it does not change their algorithms.

For 300 committed INSERTs, FullPage recovery scanned 10,040,096 bytes/1,816 records and
took 172.60 ms total (113.46 ms analysis, 2.53 ms REDO, 1,212 page writes). ByteRange
scanned 169,550 bytes/the same 1,816 records and took 11.87 ms (5.38 ms analysis,
4.16 ms REDO, the same 1,212 writes). ByteRange reduced scanning/decoding volume but
spent more time reading and patching pages during REDO. With one appended-page loser,
FullPage and ByteRange analyzed 1,818 records; total recovery was 169.47 ms versus
12.03 ms, while loser UNDO/truncation measured 0.45 ms versus 0.11 ms. These runs pass
through the production checkpoint-selection path with checkpoints disabled, so no slot
is selected and the complete retained history is exposed; checkpoint-bounded comparisons
remain separately available.

## Durability and research boundary

This experiment does not add persistent pageLSN, fuzzy checkpoints, dirty-page or
transaction tables, compensation log records, logical/operation logging, concurrency,
locks, or MVCC. Operations spanning several pages and WAL records are protected by the
existing single-statement recovery model, not a general transaction system.

ARIES is related recovery literature, but ARIES includes physiological logging,
persistent pageLSNs, repeating-history recovery, compensation records, and richer
checkpoint/concurrency machinery. MiniDB++ byte-range WAL remains simpler page-oriented,
physical before/after logging and does not claim ARIES compliance. See the citation in
[wal.md](wal.md).
