# Benchmarking and storage observability

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
| Recovery | `recovery_full_scan`, `recovery_loser`, `recovery_page_lsn_compare`, `recovery_checkpoint_compare` |

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

TCP workloads use one loopback client at a time. Their latency includes wire framing,
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

- This harness is single-process and single-client; it is not a concurrency benchmark.
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

## Controlled pre/post and replacement-policy comparisons

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
