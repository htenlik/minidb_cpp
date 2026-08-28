# Adaptive physical WAL update encoding

MiniDB++ can select the smaller of its two existing physical page-update encodings for
each emitted update record. Enable this experiment with:

```text
--wal-update-mode adaptive
```

`full-page` remains the default. Adaptive mode adds no mode-specific record type or
recovery branch: on PageLSN-aware pages the choice is `PAGE_UPDATE_V2` or
`PAGE_DELTA_UPDATE_V2`; retained legacy records keep their original types. REDO and
UNDO dispatch solely on the persisted type.

## Exact decision rule

The selector first computes the canonical byte ranges described in
[wal-byte-range.md](wal-byte-range.md), then compares complete encoded WAL record sizes:

```text
FullPage v2 = 48-byte WAL header + 8,216-byte PAGE_UPDATE_V2 payload
            = 8,264 bytes

existing-page Delta v2 = 48 + 32 + sum(4 + 2 * range.length)
new-page Delta v2      = 48 + 32 + sum(4 +     range.length)
```

The sums include the outer record header, update payload header, every four-byte range
descriptor, and all encoded before/after data. Size arithmetic is checked against
`size_t`, the 32-bit encoded lengths, and the one-MiB record limit. A malformed or
unencodable candidate is rejected.

If `Delta < FullPage`, the selector writes the delta type; otherwise it writes the full
type. An exact tie therefore deterministically selects FullPage. For one contiguous
changed run on an existing PageLSN-aware page, 4,089 bytes produce an 8,262-byte delta,
4,090 bytes tie at 8,264, and 4,091 bytes produce an 8,266-byte delta. The legacy type-
2/type-8 formula remains unchanged for retained or unsupported-format pages.

The implementation materializes the canonical range representation once, calculates
both candidate sizes, and serializes only the selected payload. It does not allocate
both an 8-KiB full-page payload and a delta payload merely to compare them.

## Record-level behavior

Selection is independent for every page-update record. One statement can therefore
contain both record types, including repeated persistence states of the same page under
STEAL. The coordinator retains the statement-start before image and cumulative
touched-byte mask, so choosing a different type later does not weaken loser UNDO.

New pages use the same comparison. Their delta omits the logically zero before bytes,
whereas the valid full-page candidate retains the established two-image layout.
Metadata page 0 also goes through the same selector; it has no special WAL encoding.

The current generation mode never affects recovery of retained history. Full-page-only,
byte-range-only, and mixed histories can be followed by adaptive transactions and
reopened under any generation mode. Database-format-v2 generation uses the PageLSN-aware
type IDs and layouts documented in [page-lsn.md](page-lsn.md); retained records and the
checkpoint/control, segment manifest/header, and wire formats are never reinterpreted.

## Diagnostics

Adaptive counters are zero in explicit FullPage and ByteRange modes:

| Metric | Meaning |
| --- | --- |
| `adaptiveFullPageSelections` | adaptive records encoded as `PAGE_UPDATE` |
| `adaptiveDeltaSelections` | adaptive records encoded as `PAGE_DELTA_UPDATE` |
| `adaptiveTies` | exact candidate-size ties; included in FullPage selections |
| `bytesIfFullPage` | sum of complete FullPage candidate bytes |
| `bytesIfDelta` | sum of complete Delta candidate bytes |
| `bytesActuallyChosen` | sum of complete selected update-record bytes |
| `bytesSavedByAdaptive` | `bytesIfFullPage - bytesActuallyChosen` |
| `bytesSavedVersusByteRange` | `bytesIfDelta - bytesActuallyChosen` |
| `deltaComputationNs` | time spent constructing canonical ranges |
| `adaptiveSelectionNs` | time spent validating/sizing ranges and selecting a type |

The public `AdaptivePageUpdateDecision` also exposes the chosen type, both exact sizes,
range count, and represented changed-byte count for tests and focused diagnostics.
Normal server operation does not log a verbose line for each choice.

## Controlled Release snapshot

The following single-run comparison was captured on 2026-08-27 on an Apple M2 with
AppleClang 21. All runs used the same warning-clean Release build, seed 12345, 64 buffer
frames, LRU-K `K=2`, 16-MiB segments, and automatic checkpoints disabled. Setup and
operation counts match the earlier byte-range experiment: 200 scalar/VARCHAR/insert/
delete operations, 16 B+ boundary inserts after 406 setup rows, and 300 mixed SQL
operations after 300 setup rows. Timing is machine-specific; encoded sizes and choices
are deterministic.

| Workload | Mode | ops/s | p95 us | p99 us | WAL bytes | updates | mean update bytes | total amp. |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| scalar update | FullPage | 3,499 | 324 | 541 | 1,673,600 | 200 | 8,256 | 4,184.00 |
| scalar update | ByteRange | 13,299 | 84 | 95 | 38,400 | 200 | 80 | 96.00 |
| scalar update | Adaptive | 13,629 | 87 | 94 | 38,400 | 200 | 80 | 96.00 |
| VARCHAR update | FullPage | 3,371 | 343 | 586 | 1,896,512 | 227 | 8,256 | 7.85 |
| VARCHAR update | ByteRange | 5,612 | 342 | 429 | 785,228 | 227 | 3,360 | 3.25 |
| VARCHAR update | Adaptive | 6,075 | 328 | 364 | 785,228 | 227 | 3,360 | 3.25 |
| INSERT | FullPage | 3,377 | 316 | 615 | 6,643,712 | 802 | 8,256 | 873.71 |
| INSERT | ByteRange | 11,173 | 103 | 108 | 111,942 | 802 | 112 | 14.72 |
| INSERT | Adaptive | 11,247 | 104 | 110 | 111,942 | 802 | 112 | 14.72 |
| DELETE | FullPage | 3,194 | 378 | 606 | 6,676,736 | 806 | 8,256 | 52.91 |
| DELETE | ByteRange | 6,080 | 355 | 427 | 619,186 | 806 | 740 | 4.91 |
| DELETE | Adaptive | 6,000 | 365 | 384 | 619,186 | 806 | 740 | 4.91 |
| B+ boundary insert | FullPage | 3,244 | 431 | 431 | 546,688 | 66 | 8,256 | 248.38 |
| B+ boundary insert | ByteRange | 8,946 | 275 | 275 | 16,367 | 66 | 221 | 7.44 |
| B+ boundary insert | Adaptive | 9,034 | 293 | 293 | 16,367 | 66 | 221 | 7.44 |
| mixed SQL | FullPage | 3,172 | 352 | 686 | 7,464,000 | 900 | 8,256 | 48.07 |
| mixed SQL | ByteRange | 6,884 | 377 | 402 | 724,326 | 900 | 767 | 4.66 |
| mixed SQL | Adaptive | 6,755 | 387 | 446 | 724,308 | 900 | 767 | 4.66 |

Across these adaptive runs, 3,000 updates selected Delta and one fragmented mixed-SQL
update selected FullPage; there were no ties. Adaptive saved 22,605,817 bytes versus
always FullPage and 18 bytes versus always ByteRange. Throughput differences between
Adaptive and ByteRange are within the limits of a single run and are not pass/fail
criteria.

Two directed 200-operation workloads isolate the decision boundary:

| Workload | Mode | WAL bytes | FullPage / Delta selections | diff ns/update | selection ns/update |
| --- | --- | ---: | ---: | ---: | ---: |
| 16 contiguous changed bytes | ByteRange | 44,000 | n/a | 2,757 | n/a |
| 16 contiguous changed bytes | Adaptive | 44,000 | 0 / 200 | 2,728 | 20 |
| 2,048 alternating changed bytes | FullPage | 1,673,600 | n/a | n/a | n/a |
| 2,048 alternating changed bytes | ByteRange | 2,494,400 | n/a | 68,205 | n/a |
| 2,048 alternating changed bytes | Adaptive | 1,673,600 | 200 / 0 | 68,609 | 7,410 |

Adaptive saved 820,800 bytes versus always ByteRange in the fragmented workload. It is
not required to outperform either explicit mode: only the per-record minimum-size
invariant is normative.

With 400 mixed operations, 16-KiB segments, and a sharp checkpoint every 50 statements,
all modes completed eight checkpoints. FullPage/ByteRange/Adaptive appended
9,936,896/1,083,040/1,083,022 bytes and rotated 1,198/80/80 times; Adaptive selected
1 FullPage and 1,197 Delta records. Reclamation retained 25,360 bytes for FullPage and
21,796 bytes for both other modes.

For equivalent 300-INSERT histories, FullPage/ByteRange/Adaptive recovery scanned
10,040,096/169,550/169,550 bytes and 1,816 records. Total recovery times were
173.01/11.76/11.78 ms, with 1,212 recovery writes in every mode. Adaptive recovery has
no special path; this result follows from its chosen persisted record types.

## Testing and current boundary

The selector suite checks exact encoded-size agreement for 2,000 deterministic pairs
and the minimum-size invariant for 10,000 pairs using seed `0x11D2A11`. Inputs include
sparse, contiguous, random-density, alternating, and whole-page changes. Crash tests
run SQL mutation cases under all three modes with a small STEAL-capable pool, include a
single mixed adaptive transaction before/after COMMIT, and force that mixed chain across
8,300-byte WAL segments. Checkpoint, reclamation, and lost-control rebuild tests also
run in Adaptive mode.

Persistent PageLSN now enables selective REDO, but there is still no fuzzy checkpoint,
dirty-page table, compensation log record, concurrency, locking, MVCC, or crash-atomic
checkpoint/update protocol beyond the documented statement model. Adaptive mode computes
canonical deltas even when it later chooses FullPage, trading CPU work for bounded WAL
volume.
