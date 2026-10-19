# Benchmarking and storage observability

## Frame dependency retirement experiment

`frame_wal_retirement_benchmark` takes 30 samples per case (`--smoke`: 3), using
Adaptive WAL, 9000-byte segments and enough frames for 1/100/1000 written pages;
the eviction case uses two frames. Mutation/commit/writeback precedes timing.
The mixed case adds a still-dirty page. Repeated-fuzzy/sharp cases alternate
mutation and the selected checkpoint, without turning fuzzy into sharp.
An eight-thread READ ONLY benchmark separately executes 8,000 SELECTs and checks
zero transaction WAL growth.

The identical benchmark source was also compiled with `MINIDB_RETIREMENT_BASELINE`
against clean baseline `2e55271` headers/libraries. That switch only omits new
diagnostics and the known-failing post-reclamation validation on the baseline;
workload, reclamation, bytes and timing boundaries stay identical. It is not a
production option or a weakened correctness test.

Local macOS arm64, AppleClang 21, warning-enabled Release observations, with other
verification jobs running. Nearest-rank microseconds; p99 with 30 samples is the
maximum and not a reliable tail estimate. These noisy runs do not establish a
speedup, regression estimate, isolated fsync cost or universal throughput claim.

| Case | Baseline p50/p95/p99 us | Repaired p50/p95/p99 us | Mean retirement us | Retired / renewed |
| --- | ---: | ---: | ---: | ---: |
| 1 clean | 939.750 / 1250.08 / 1496.17 | 619.625 / 893.209 / 985.875 | 2.926 | 30 / 29 |
| 100 clean | 1124.79 / 1766.08 / 2506.62 | 983.834 / 1399.21 / 1496.79 | 17.825 | 3000 / 2900 |
| 1000 clean | 3241.12 / 4127.21 / 4597.50 | 4003.33 / 6384.21 / 16303.3 | 365.204 | 30000 / 29000 |
| Mixed | 971.416 / 1281.21 / 1283.04 | 1129.88 / 1577.25 / 1627.92 | 22.424 | 3000 / 2900 |
| Repeated fuzzy | 655.875 / 902.916 / 960.917 | 631.583 / 857.541 / 1170.46 | 1.241 | 30 / 29 |
| Repeated sharp | 610.500 / 746.375 / 954.625 | 602.709 / 797.541 / 1052.67 | 0.873 | 30 / 29 |
| Eviction | 654.167 / 967.708 / 992.875 | 677.625 / 1363.21 / 1446.33 | 0.905 | 0 / 0 |

Every case in both versions performed **90 WAL fsyncs, 30 database fsyncs and
zero timed database page writes**. Retirement adds metadata work, not data writes
or fsyncs. Baseline retirement counts are unavailable, not measured zero work.
The repaired hit ratio over setup/mutation/read requests was 0.966667 (0 for the
eviction workload). The SELECT runs observed 22,164 baseline vs 23,114 repaired
queries/s, with no inferred speedup.

| Case | Retained WAL bytes (both) | Actually deleted segments (both) | Baseline / repaired reopen ms |
| --- | ---: | ---: | ---: |
| 1 clean | 1160 | 28 | 1.310 / 0.631 |
| 100 clean | 15360 | 58 | 2.893 / 1.913 |
| 1000 clean | 15296 | 508 | 1.456 / 2.963 |
| Mixed | 15636 | 58 | 1.725 / 1.711 |
| Repeated fuzzy | 1160 | 28 | 0.683 / 0.668 |
| Repeated sharp | 1128 | 28 | 0.658 / 0.783 |
| Eviction | 1160 | 28 | 0.545 / 1.077 |

Reopen times include fixture construction/recovery/teardown. Retirement time
includes preflight after metadata-lock acquisition. Global retained bytes and
deletion counts match the baseline: no keep-everything retention workaround.
See [frame lifecycle/proof/tests](frame-wal-dependency-retirement.md).

## Physical writeback frontier experiment

Build/run `physical_writeback_benchmark` in Release, or use `--smoke` for three
samples per case. The full run takes 30 checkpoints per deterministic workload,
Adaptive WAL, 9000-byte segmented payloads and 128 buffer frames (one for the
eviction case). Pending writes are prepared before timing. No test shadow-copy
observer is installed in this benchmark. Timed checkpoints do not flush buffers.

Observed on local macOS arm64, AppleClang 21 Release, while other verification
jobs were active. Microseconds below are empirical nearest-rank quantiles; with
30 samples p99 is the maximum, not a strong tail-latency estimate.

| Case | p50 us | p95 us | p99 us | DB fsyncs | WAL fsyncs | Checkpoint DB writes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| No pending writeback | 498.541 | 824.542 | 1104.83 | 0 | 90 | 0 |
| One clean resident writeback | 533.875 | 733.917 | 913.666 | 30 | 90 | 0 |
| 100 distinct page writebacks | 844.750 | 1387.42 | 3113.17 | 30 | 90 | 0 |
| Evicted writeback | 524.417 | 550.458 | 1099.46 | 30 | 90 | 0 |
| Direct page-0 writeback | 522.708 | 552.375 | 615.542 | 30 | 90 | 0 |
| One written + one still-dirty page | 1096.38 | 2506.50 | 2509.46 | 30 | 90 | 0 |

| Case | Mean pending epoch distance | Mean DPT entries | Retained WAL bytes | Segments deleted | Reopen/recovery ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| No pending | 0 | 0 | 640 | 28 | 0.437459 |
| One clean | 1 | 0 | 1160 | 28 | 0.624917 |
| 100 writebacks | 100 | 0 | 15360 | 58 | 1.203000 |
| Evicted | 1 | 0 | 1160 | 28 | 0.445500 |
| Page zero | 1 | 0 | 1048 | 28 | 0.519375 |
| Mixed dirty | 1 | 1 | 23888 | 0 | 4.783580 |

Counts cover the 30 timed checkpoints, not setup. WAL counts include checkpoint
forces and segment rotation syncs. Recovery timing includes opening/recovery and
teardown of the test fixture, not a pure REDO-loop measurement. Retained size and
deletions are observed before closing the first fixture. The mixed case deliberately
keeps a dirty period open, so its recLSN protects history; other cases demonstrate
real bounded reclamation. A later sharp checkpoint can flush that dirty period.

These cases measure the corrected conditional fence, not a claimed speedup over
the unsafe implementation. Some fuzzy checkpoints now incur database fsync; no
pending writebacks incur none, and 100 writes share one. Timing differences are
not isolated fsync-cost estimates or universal performance claims. See
[physical writeback durability](physical-writeback-durability.md) for the protocol
and [buffer WAL retention](buffer-wal-retention.md) for the separate frame-local
dependency lifecycle.

## Reclaim lineage recovery experiment

Build and run `reclaim_lineage_benchmark` in Release. It exercises 1/10/100/1000
reclaim/reuse cycles, zero/one/two intermediate CLR/reuse cycles, unrelated-page WAL
traffic and both single-file/segmented WAL. A cycle is not one WAL record: the output
reports the actual scanned records and validated transitions. Every measured history
must recover successfully. Columns include recovery microseconds, lineage/CLR steps,
accounted lineage and LSN-index bytes, PageLSN skips and physical database writes.
Memory accounting excludes scan payloads, allocator/node overhead and RSS.

One local AppleClang 21 Release run (other validation workloads running concurrently)
measured segmented 1000-cycle recovery at 153/716/1271 ms for zero/one/two CLR cycles,
with 7,998/13,998/19,998 scanned records and 4.18/8.35/12.53 MB accounted lineage storage.
The separate `records_analyzed` counter excludes system reclaim records; it is not
the total scan count.
These are observations, not throughput promises or isolated-machine comparisons.

`reclaim_lineage_test --frame-diagnostic` is now a passing independent regression
for [frame retirement](frame-wal-dependency-retirement.md). The lineage experiment
itself does not implement retirement.

The harness covers the legacy Pager, bounded buffer pool, storage/index/SQL paths,
standalone WAL append/force behavior, durable transaction and recovery paths, sharp and
fuzzy checkpoints, segment rotation/reclamation, and full-page versus physical byte-range
logging. It reports measurements, not performance claims.
Results are meaningful primarily when comparing the same machine, compiler, build type,
configuration, and seed.

## Storage backends

> The legacy Pager retains accessed pages in an unbounded in-memory cache for the
> lifetime of the Pager.

This cache has no capacity limit, replacement policy, pin/unpin protocol, eviction,
dirty-victim flushing, or page guards. It is therefore not a database buffer pool.
Touching distinct pages causes `residentPages` to grow until the Pager is destroyed.
Only `pager_*` workloads use this historical backend. They are labeled
`storage_backend = "legacy_pager"`.

Standalone `buffer_*` and active B+ tree, TupleStore, SQL, TCP, and mixed workloads use
BufferPoolManager + DiskManager and are labeled `storage_backend = "buffer_pool"`.
Their resident set is capped by `--buffer-frames`, and `--lru-k` controls replacement.
Standalone `wal_*` workloads use only LogManager and are labeled
`storage_backend = "wal"`; they are not full transactions or SQL operations.

Instrumentation excludes the database metadata page read performed by Pager startup.
It does not change allocation, loading, dirtying, or flushing behavior. `PagerStats`
uses 64-bit counters with these exact meanings:

| Field | Meaning |
| --- | --- |
| `pageRequests` | Valid normal data-page calls to `getPage()` |
| `cacheHits` | Requested data page already had a resident frame |
| `cacheMisses` | Requested data page needed a file load |
| `physicalPageReads` | Completed 4096-byte data-page reads from the database file |
| `physicalPageWrites` | Completed 4096-byte dirty data-page writes |
| `dirtyMarks` | Successful calls to `markDirty()`, including repeated marks |
| `flushCalls` | Valid `flush(PageId)` invocations reaching flush logic, including clean/nonresident pages and an invocation whose later file write fails |
| `appendedPages` | Pages appended by `allocatePage()`; allocator reuse is not an append |
| `residentPages` | Current frame count; during aggregation, the maximum across reopen segments |

`Pager::stats()` returns a snapshot. `Pager::resetStats()` zeroes event counters but
does not clear resident frames; the returned `residentPages` gauge still reflects them.
`Pager::residentPageCount()` exposes the same current gauge independently.

Buffer-backed storage reporting uses DiskManager, BufferPoolManager, and PageAllocator;
legacy workloads use Pager. Results contain database page count, file bytes, free-page
count, and resident-frame count immediately before and after the measured workload.

## Methodology

`page_reclaim_benchmark` compares ordinary release with explicit durable data completion,
1/10/100 synchronous system reclaims, reclaim+reuse, and recovery after one interrupted
two-page publication. It reports p50/p95/p99, throughput, actual WAL bytes/fsyncs, direct
and buffered database writes, explicit data fsyncs, list length, extent and recovery
PageLSN skips. System-only records cost 96 bytes each; reuse has additional user WAL.
Ordinary release's comparison includes commit, buffer flush and data fsync, not just
the method call. The structural/model tests and benchmark use controlled unreferenced
physical fixtures, not arbitrary production reachability claims. See
[crash-safe-page-reclamation.md](crash-safe-page-reclamation.md).

`durable_page_append_benchmark` runs 20 samples each of existing-page UPDATE, first
append, four appends, and 16 allocation-heavy INSERTs in one explicit transaction,
for FullPage/ByteRange/Adaptive. Setup/header fsyncs are excluded. It reports BEGIN and
PAGE_RESERVE counts, transaction WAL fsyncs, reservation/total record bytes, mutation
batch p95, per-append p95/p99 and commit p95. SQL INSERT percentiles include full SQL
execution, not allocator-only latency. Throughput excludes setup and includes commit;
low-level throughput counts transactions, SQL throughput counts rows. No timings are
pass/fail thresholds. The implementation combines BEGIN+reservation in one first
force; separate forces were not benchmarked. See
[durable-page-reservation.md](durable-page-reservation.md) for correctness boundaries.

Use a Release build for representative measurements:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Each repetition removes the configured benchmark database, creates deterministic setup
data, optionally warms relevant hot reads, resets backend counters, measures operations,
and validates state outside the timed region. The database is removed afterward unless
`--retain-db` is supplied. Insertion workloads deliberately time insertion into an
otherwise empty structure; lookup/update/delete/scan workloads populate first without
timing setup.

Every operation is timed independently using `std::chrono::steady_clock`. Reported
`total_ns` is the sum of those measured intervals; mean and throughput derive from that
sum. The timer-call overhead is therefore present, and nanosecond values should not be
interpreted as precision guarantees. No workload currently uses batch-derived latency.

WAL operations separately retain append and synchronous force samples. Their overall
record latency includes the force selected by the workload; a final force is charged to
the final record so records/s and payload bytes/s include clean durability completion.
Latency percentiles use nearest rank: sort `N` samples, compute
`rank = ceil(percent * N / 100)`, constrain rank to `1..N`, and return element
`rank - 1`. The tool reports count, total, operations/second, mean, p50, p95, p99,
minimum, and maximum in nanoseconds.

Fixed seeds make data generation, key order, and operation selection reproducible.
The default seed is `12345`. Tuple payload bytes and sizes are derived from the seed and
tuple index. The size modes are `small` (1–64 bytes), `medium` (129–512), `large`
(513–1500), and `mixed` (all ranges plus 17–128-byte tuples).

### Hot and reopen modes

`--mode hot` retains one backend owner. Relevant point-read workloads can perform
`--warmup N` untimed operations before counters reset. `--mode reopen` flushes through
normal owners where applicable, destroys and recreates the Pager or DiskManager/buffer
pool/component owner every
`--reopen-interval N` measured operations, and aggregates counters across segments.

Reopen clears MiniDB++'s cache. It is **not** a cold-disk benchmark: the operating
system may retain file pages. The harness never drops OS caches or requires privileges.

`--working-set N` limits the key/page population sampled by supported lookup workloads;
zero means the whole available dataset. Choose both a hot-set run where the sampled
pages fit within frame capacity and a memory-bound run where they do not.

## Workload definitions

The executable supports family aliases (`pager`, `buffer`, `bplus`, `tuple`, `sql`,
`tcp`, `mixed`, and `wal`) and these named workloads:

| Family | Workloads and measured region |
| --- | --- |
| Pager | `pager_sequential`, `pager_random`, `pager_hot`: reads of pre-created pages; hot preloads its working set |
| Bounded buffer | `buffer_sequential`, `buffer_random`, `buffer_hotset`, `buffer_scan_resistance` |
| Persistent B+ tree | `bplus_insert_sequential`, `bplus_insert_random`, `bplus_find_hit`, `bplus_find_miss`, `bplus_range`, `bplus_mixed` |
| TupleStore | `tuple_insert`, `tuple_lookup`, `tuple_update`, `tuple_erase`, `tuple_scan`, `tuple_fragmentation` |
| Local SQL | `sql_pk_lookup`, `sql_heap_scan`, `sql_insert`, `sql_update`, `sql_delete`, `sql_mixed`, `sql_pk_vs_heap` |
| TCP loopback | `tcp_pk_lookup`, `tcp_heap_scan`, `tcp_insert`, `tcp_mixed` |
| Mixed profiles | `mixed_read_heavy`, `mixed_write_heavy` (local SQL) |
| WAL substrate | `wal_append_buffered`, `wal_append_flush_each`, `wal_batch_flush`, `wal_segment_rotation`, `wal_reclamation` |
| Durable statements | `txn_insert`, `txn_update`, `txn_varchar_update`, `txn_delete`, `txn_bplus_insert`, `txn_mixed` |
| Recovery | `recovery_full_scan`, `recovery_loser`, `recovery_page_lsn_compare`, `recovery_checkpoint_compare`, `recovery_clr_resume` |

`wal_append_buffered` appends all records then performs one final synchronous force.
`wal_append_flush_each` forces every record. `wal_batch_flush` forces every
`--wal-batch-size N` records plus any final partial batch. `--wal-payload-bytes N`
controls opaque payload size (up to the 1 MiB record limit), and `--wal-buffer-bytes N`
controls LogManager's memory buffer. Useful payload experiments include 32, 128, 512,
4096, and 8192 bytes. A record larger than the configured buffer exercises the direct
write path. These numbers characterize the WAL substrate only and must not be presented
as transaction throughput or evidence for a future logical/full-page logging choice.

`wal_segment_rotation` forces records through deliberately small persisted test
segments and reports rotations plus p95/p99 operation latency. `wal_reclamation`
generates many segments, deletes a reclaimable prefix, and reports before/after bytes,
deleted segments, reclaimed bytes, and deletion latency. Configure both with
`--wal-segment-bytes`; small test segments are not production 16-MiB performance.

B+ insertion uses deterministic unique 32-bit keys and valid synthetic RIDs; the other
B+ workloads populate the persistent tree first. Tuple fragmentation alternates
delete/reuse/update/scan operations over deterministic variable-length bytes. SQL uses:

```sql
CREATE TABLE users (
    id UINT32 PRIMARY KEY,
    username VARCHAR(64) NOT NULL,
    score INT64,
    active BOOLEAN NOT NULL
);
```

`sql_pk_vs_heap` runs indexed `id = ?` and non-indexed `username = ?` lookup workloads
separately on equivalent deterministic datasets. It reports measured behavior rather
than asserting one must always be faster. SQL and TCP results retain average executor
`rowsExamined` and `indexLookups`.

The standard SQL and TCP mixed profile is 70% primary-key reads, 10% scan reads, 10%
inserts, 5% updates, and 5% deletes. `mixed_read_heavy` is 90% primary-key reads, 5%
scan reads, 2% inserts, 2% updates, and 1% deletes (95/5 read/write).
`mixed_write_heavy` is 45% primary-key reads, 5% scans, 20% inserts, 15% updates, and
15% deletes (50/50). A seeded live-key model keeps targets meaningful.

The original `tcp_*` workloads use one loopback client at a time. Their latency includes wire framing,
socket transfer, server execution, and durable WAL COMMIT for mutations; database pages
are not forced per response. They must not be compared to local SQL without accounting
for those semantics.

`txn_*` workloads use the production recovery-enabled `DatabaseServer` graph without
TCP. They report statement percentiles, WAL/fsync and buffer counters, observed logical
page-byte transitions, update payload/total bytes, per-mode update counts, record-size
percentiles, delta range distribution, and delta-computation nanoseconds. Payload and
total amplification divide measured bytes by observed transitions; they do not use a
tuple-size estimate and are not universal efficiency claims. Their `wal_bytes_scanned`
is the logical tail that a restart at the latest currently selectable checkpoint would
scan; it is a projected byte count, not a timed recovery run. `recovery_full_scan`
builds deterministic committed history and measures winner REDO; `recovery_loser` adds
an uncommitted appended page and measures loser UNDO/truncation. Both validate through
normal reopen.
`recovery_page_lsn_compare` creates one PageLSN-aware page and a deterministic committed
update history, persists 0–100% of that history according to
`--redo-persisted-percent`, clones identical database/WAL inputs, and recovers one clone
with selective PageLSN REDO and one with `AlwaysRedo`. FullPage, ByteRange, and Adaptive
generation modes are supported. The benchmark validates byte-identical final pages; it
is a controlled recovery-I/O comparison, not a claim that selective REDO must improve
wall-clock time on every filesystem/cache state.
There are no CI timing thresholds.

`recovery_clr_resume` creates one loser with `--operations N` repeated physical page
updates. `--redo-persisted-percent` selects the fraction of UNDO work precompleted by
deliberately interrupted recovery runs (normally 0, 25, 50, or 75). The final restart
reports original-versus-remaining records, CLR count and bytes, cumulative interrupted/
final recovery timing, CLR PageLSN skips, and original/retained physical WAL bytes.
Counts such as 10, 100, and 1000 expose the simplicity and WAL/fsync cost of the
full-page CLR. The restart-from-original comparison is modeled as `N` revisited records;
the implementation should visit `N - completed` original records on the final restart.
No universal latency improvement is asserted because repeated fsync and cache state can
dominate small runs.

All workloads validate their relevant storage/tree/catalog/model after measurement.

## Suites and commands

Both `quick` and `baseline` contain:

```text
pager_sequential  pager_random  buffer_random  bplus_find_hit  tuple_lookup
sql_pk_lookup     sql_heap_scan sql_mixed       tcp_pk_lookup   wal_append_buffered
```

`quick` caps rows at 32, operations/pages at 24, warmup/buffer frames at 8, and reopen cadence at 12.
`baseline` uses the supplied values (defaults are 1000 rows, operations, and pages).

```bash
# Wiring/correctness check
./build-release/minidb_bench --suite quick --seed 12345

# Curated local baseline
./build-release/minidb_bench --suite baseline --seed 12345 \
  --json benchmarks/results/baseline.json

# Larger low-level and SQL samples
./build-release/minidb_bench --benchmark pager_random --pages 10000 \
  --operations 50000 --working-set 1000 --seed 12345 \
  --json benchmarks/results/pager-random.json
./build-release/minidb_bench --benchmark sql_pk_vs_heap --rows 100000 \
  --operations 50000 --seed 12345

# Synchronous WAL batch-size comparison (same payload/count/build/machine)
./build-release/minidb_bench --benchmark wal_batch_flush --operations 10000 \
  --wal-payload-bytes 128 --wal-batch-size 1
./build-release/minidb_bench --benchmark wal_batch_flush --operations 10000 \
  --wal-payload-bytes 128 --wal-batch-size 10
./build-release/minidb_bench --benchmark wal_batch_flush --operations 10000 \
  --wal-payload-bytes 128 --wal-batch-size 100

# Full-page transaction and full-history recovery baselines
./build-release/minidb_bench --benchmark txn_mixed --rows 1000 --operations 1000
./build-release/minidb_bench --benchmark recovery_full_scan --operations 1000

# Controlled WAL encoding comparison: change only --wal-update-mode
./build-release/minidb_bench --benchmark txn_update --rows 1000 --operations 1000 \
  --buffer-frames 64 --lru-k 2 --seed 12345 --wal-update-mode full-page
./build-release/minidb_bench --benchmark txn_update --rows 1000 --operations 1000 \
  --buffer-frames 64 --lru-k 2 --seed 12345 --wal-update-mode byte-range
./build-release/minidb_bench --benchmark txn_update --rows 1000 --operations 1000 \
  --buffer-frames 64 --lru-k 2 --seed 12345 --wal-update-mode adaptive

# Other logging-comparison categories
./build-release/minidb_bench --benchmark txn_varchar_update --rows 1000 --operations 1000
./build-release/minidb_bench --benchmark txn_insert --operations 1000
./build-release/minidb_bench --benchmark txn_delete --rows 1000 --operations 1000
./build-release/minidb_bench --benchmark txn_bplus_insert --rows 406 --operations 16
./build-release/minidb_bench --benchmark txn_mixed --rows 1000 --operations 1000

# Directed adaptive-selection workloads
./build-release/minidb_bench --benchmark txn_wal_delta_friendly --operations 1000 \
  --wal-update-mode adaptive
./build-release/minidb_bench --benchmark txn_wal_fragmentation --operations 1000 \
  --wal-update-mode adaptive

# Controlled sharp/fuzzy checkpoint cost; operations is prepared dirty-page count
./build-release/minidb_bench --benchmark checkpoint_latency --operations 256 --checkpoint-mode sharp
./build-release/minidb_bench --benchmark checkpoint_latency --operations 256 --checkpoint-mode fuzzy
./build-release/minidb_bench --benchmark recovery_checkpoint_compare --operations 1000 --checkpoint-mode fuzzy

# Repeated fuzzy checkpoints: 100 regularly flushes, 0 retains dirty periods
./build-release/minidb_bench --benchmark checkpoint_retention --rows 16 --operations 64 \
  --checkpoint-mode fuzzy --redo-persisted-percent 100
./build-release/minidb_bench --benchmark checkpoint_retention --rows 16 --operations 64 \
  --checkpoint-mode fuzzy --redo-persisted-percent 0

# Selective PageLSN REDO versus AlwaysRedo over identical history
./build-release/minidb_bench --benchmark recovery_page_lsn_compare --operations 1000 \
  --wal-update-mode adaptive --redo-persisted-percent 0
./build-release/minidb_bench --benchmark recovery_page_lsn_compare --operations 1000 \
  --wal-update-mode adaptive --redo-persisted-percent 50
./build-release/minidb_bench --benchmark recovery_page_lsn_compare --operations 1000 \
  --wal-update-mode adaptive --redo-persisted-percent 100
```

See [the benchmark command reference](../benchmarks/README.md) for every option.

## JSON schema version 1

The output root is `{"schema_version":1,"results":[...]}`. Each result contains:

- `benchmark`, explicit `storage_backend`, `seed`, and one-based `repetition`;
- `configuration`: rows, operations, pages, working set, warmup, reopen interval,
  repetitions, buffer frames, LRU-K K, WAL payload/batch/buffer/segment sizes, WAL
  update/checkpoint modes, persisted-REDO percentage, cache mode, and tuple sizes;
- `timing`: operation count, total, throughput, mean, p50/p95/p99, min/max;
- `pager`: all nine Pager statistics;
- `buffer`: bounded-buffer requests, hits/misses, derived hit ratio, physical I/O,
  evictions, pin activity, appended pages, WAL force requests, gauges, and capacity;
- `wal`: record/payload counts, payload throughput, encoded bytes written, physical
  retained bytes before/after reclamation, logical end, active/retained segment identities, rotations/deletions,
  buffer drains, force requests/fsyncs, last/durable LSN, and append/force p95/p99;
- `recovery`: transaction/per-encoding update counters, observed/represented bytes,
  payload and total WAL amplification, record-size and range distributions, delta CPU
  nanoseconds, adaptive choice/tie/candidate/saved-byte and selection-time counters,
  runtime persistent-PageLSN assignments/v1 observations/known-v2 observations,
  checkpoint use/skipped/scanned bytes, full-scan comparison, scanned/
  REDO/UNDO counts, DPT candidate/absence/before-recLSN skips, PageLSN
  checks/unknowns/skips/checked applies, legacy replays,
  recovery reads/writes, AlwaysRedo comparison counters, and phase/total recovery
  nanoseconds;
- `checkpoint`: sharp/fuzzy counts, DPT entries/oldest recLSN/retention floor, dirty
  writes, WAL/database/control syncs, checkpoint
  latency, and separately reclaimed segments/bytes/reclamation latency; configuration
  records byte/statement thresholds and enablement;
- `transaction`: generated WAL bytes, WAL/COMMIT fsync counts, terminal COMMIT or
  ROLLBACK latency, touched pages, original-before-image pages/bytes, estimated peak
  recovery-context bytes, pending/deferred checkpoint flags, and retained WAL peak/after;
- `storage.before` and `storage.after`: pages, bytes, free and resident pages;
- `execution`: average rows examined and index lookups;
- `environment`: version context, configured Git commit, compiler, build type, platform,
  C++ standard, page size, protocol version, and hardware concurrency;
- `validation_passed`.

Strings are escaped by the project-native writer; no external JSON dependency is used.
Generated `.json`, `.benchmark.json`, and benchmark `.db` files are ignored. The
repository commits definitions and schema, not machine-specific performance numbers.
A comparison parser/tool is deferred; compare JSON only across controlled equivalent
machine/build configurations.

The `txn_wal_delta_friendly` workload flips one 16-byte contiguous run in a raw allocated
page per statement. `txn_wal_fragmentation` flips alternating bytes, producing 2,048
canonical one-byte ranges. These are controlled physical logging experiments; they do
not represent SQL throughput. See [wal-adaptive.md](wal-adaptive.md) for the three-way
Release snapshot and recovery comparison.
Persistent PageLSN metric semantics and the relationship to sharp checkpoints are in
[page-lsn.md](page-lsn.md).

## Limitations and interpretation

### Explicit transaction experiments

`transaction_implicit` and `transaction_explicit` execute identical SQL updates of one
INT64 field in a pre-created single-row table. `--operations 1000` means 1,000 distinct
values, either 1,000 autocommits or one BEGIN/COMMIT scope. Setup and its sharp/fuzzy
checkpoint are untimed. Statement percentiles include implicit commit but exclude the
explicit terminal COMMIT; throughput uses total time including BEGIN and completion.
`transaction.final_commit_ns` reports explicit completion separately. Ordinary automatic
checkpoints are disabled for this comparison. This is a hot, serial, same-page workload,
not evidence of a universal throughput improvement.

```bash
./build-release/minidb_bench --benchmark transaction_implicit --operations 1000 --json implicit.benchmark.json
./build-release/minidb_bench --benchmark transaction_explicit --operations 1000 --json explicit.benchmark.json
./build-release/minidb_bench --benchmark transaction_rollback --operations 1000
./build-release/minidb_bench --benchmark transaction_recovery --operations 1000
./build-release/minidb_bench --benchmark transaction_checkpoint --operations 1000 \
  --checkpoint-wal-bytes 4096 --wal-segment-bytes 16384 --checkpoint-mode sharp
```

Run rollback/recovery at 10, 100, and 1,000 updates. Live rollback reports terminal
latency, CLR count/bytes, compensation writes, and truncation operations in `recovery`.
Recovery forks a writer, flushes loser pages under STEAL, exits without destructors,
then measures real startup analysis/REDO/UNDO and total open/validation/close time.
`recovery.total_ns` isolates the recovery phases from that enclosing wall time.

The checkpoint workload verifies pending=true and zero published checkpoints while
active, then exactly one checkpoint after COMMIT. Repeat with `--checkpoint-mode fuzzy`.
Retained peak includes buffered WAL; retained-after includes completion and optional
reclamation, so fuzzy retention may remain high while its DPT still needs history.
Before-image metrics exclude map allocator overhead, temporary encodings, and global
statistics vectors; they are recovery-state estimates, not RSS. See
[transactions.md](transactions.md) for the long-transaction memory and preparation costs.

### General limitations

- Original workload families use one client; `concurrency_*` explicitly measure concurrent clients.
- WAL batch forcing is synchronous and single-threaded; it is not group commit.
- Per-operation timer overhead matters for very cheap cache hits.
- Reopen mode cannot bypass the OS page cache.
- The unbounded cache can consume memory proportional to distinct pages accessed.
- First-fit storage behavior, server flush policy, validation, and current SQL features
  are intentionally measured as implemented, not optimized away.
- Git metadata is injected simply at CMake configure time and does not encode worktree
  cleanliness; reconfigure after changing commits for an exact identifier.
- No automatic cross-run comparison or timing-regression threshold is included.

The legacy unbounded Pager and bounded pool are not equivalent memory configurations.
See [buffer-pool.md](buffer-pool.md) for exact LRU-K, guard, flush, metric, and
scan-resistance definitions.

## Concurrent reader experiments

`concurrency_pk_read` and `concurrency_heap_read` measure real loopback TCP SELECTs
using `--client-threads N` (1..64). Run 1/2/4/8/16 against the same immutable rows,
operation count, seed, frame capacity, and build. Setup inserts 96-byte payload rows
and completes a sharp checkpoint before measurement. Connections/handshakes are
untimed; warmup is zero and automatic checkpoints are disabled. Requests are partitioned
by operation number so all thread counts issue the same query set.

```bash
./build-release/minidb_bench --benchmark concurrency_pk_read --rows 1024 \
  --operations 512 --client-threads 8 --buffer-frames 64 --json readers.benchmark.json
./build-release/minidb_bench --benchmark concurrency_heap_read --rows 1024 \
  --operations 512 --client-threads 8 --buffer-frames 64
./build-release/minidb_bench --benchmark concurrency_buffer_reads --rows 1024 \
  --operations 512 --client-threads 8 --buffer-frames 16
./build-release/minidb_bench --benchmark concurrency_read_only --rows 1024 \
  --operations 512 --client-threads 8 --buffer-frames 64
./build-release/minidb_bench --benchmark concurrency_serial_read --rows 1024 \
  --operations 512 --client-threads 8 --buffer-frames 64
./build-release/minidb_bench --benchmark concurrency_writer_exclusion --rows 1024 \
  --client-threads 8 --writer-hold-ms 20 --buffer-frames 64
```

`concurrency_read_only` retains one READ ONLY scope per client batch.
`concurrency_serial_read` issues identical queries in READ WRITE scopes, deliberately
serializing those batches through exclusive access. This is a reproducible scheduling
baseline on the current engine, not a historical binary performance comparison. Both
include BEGIN/COMMIT in total wall time; SELECT percentiles exclude those controls and
`concurrency.begin_p95_ns` separately reports BEGIN wait/response latency.

`concurrency_buffer_reads` uses the PK SQL/TCP path under small-buffer pressure; repeat
with 16/32 frames and 1/2/4/8 clients. Buffer metadata, LRU-K, and miss I/O are serialized;
current Table::open validation also examines storage each request. These are whole-engine
costs, not isolated B+ lookup times, and additional readers need not improve throughput.

`concurrency_writer_exclusion` queues exactly one reader request per client behind a
READ WRITE update, verifies all readers are waiting, holds another `--writer-hold-ms`
(0..1000), then commits. Effective operation count is the client count. It reports
request wait/response latency, total writer lifetime from BEGIN through COMMIT response,
gate reader waiting nanoseconds, and first-to-last completion spread. No blocked reader
observes the intermediate value.

The additive `concurrency` JSON object includes gate admissions/waits/peak shared leases,
peak/active sessions, rejections/cancellations, writer duration, completion spread, and
BEGIN p95. `buffer` adds frame-availability wait calls and content-latch contention
counts. Gate readers are lease holders, not a CPU-utilization measurement. Throughput
uses wall time through final client completion; request percentiles use individual
samples. `timing.mean_ns` remains wall-time per operation, not concurrent request mean.
Machine-specific reports stay outside tracked files; there are no timing assertions
or linear-scaling claims. See [concurrency-baseline.md](concurrency-baseline.md).

## Controlled pre/post and replacement-policy comparisons

### Multi-transaction recovery substrate

Build/run `multi_recovery_benchmark` (also a CTest smoke target). It uses real WAL
codecs and disjoint preallocated pages, not concurrent SQL writers. Analysis samples
use 1/10/100/1000 mixed transactions; UNDO samples use 128 total updates distributed
over 1/2/4/8/16 losers. An eight-loser subprocess is interrupted after 32/64/96 durable
compensations and restarted. The retention experiment holds an old active transaction
over ten fuzzy checkpoints and 1000 newer synthetic commits, then releases it.

One AppleClang Release run reported analysis 0.064/0.270/2.386/24.001 ms with
3/30/300/3000 analyzed records and accounted LSN-index bytes 72/728/7256/72008.
Equal-work UNDO took 21.408/20.930/22.585/21.246/22.653 ms; each appended 128 CLRs,
535552 CLR WAL bytes and wrote 128 pages. Queue pops were 129/130/132/136/144.
Restart at 25/50/75% appended only the remaining 96/64/32 CLRs and took
18.271/17.607/12.879 ms. ATT retained floor 64 across the active lifetime; after
completion the floor advanced to 192660, retained segments fell 31 -> 3 and physical
WAL bytes 194596 -> 1812. These are single-machine observations, not scalability
claims or timing assertions. The executable prints fresh measurements on every run.
Index accounting excludes allocator overhead and decoded record payloads.
See [multi-transaction-recovery.md](multi-transaction-recovery.md).

### Earlier implementation comparisons

Capture machine-specific JSON under ignored `benchmarks/results/`. A 10B comparison
uses the same Release compiler/build, database sizes, operation counts, workload
definitions, seed, cache mode, and machine for the pre-migration commit and migrated
commit. Report throughput and p95/p99 alongside resident/capacity, hits/misses, physical
I/O, and evictions. The old unbounded cache and bounded pool answer different memory
constraints, so neither should be described as universally faster.

For a meaningful replacement-policy experiment, run one full-engine read-heavy workload
twice with identical data, operations, seed, frame capacity, and working set, changing
only K=1 versus K=2. A single run demonstrates behavior but is not statistically
significant. Reopen mode still cannot control the operating-system page cache.
