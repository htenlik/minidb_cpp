# Benchmarking and storage observability

Milestone 9 establishes a repeatable measurement baseline after the `v0.1.0` MVP and
before the bounded buffer pool. It adds counters and workloads, not performance claims.
Results are meaningful primarily when comparing the same machine, compiler, build type,
configuration, and seed.

## Current Pager model

> The v0.1.x Pager retains accessed pages in an unbounded in-memory cache for the
> lifetime of the Pager.

This cache has no capacity limit, replacement policy, pin/unpin protocol, eviction,
dirty-victim flushing, or page guards. It is therefore not a database buffer pool.
Touching distinct pages causes `residentPages` to grow until the Pager is destroyed.
Milestone 10 can use the same workloads to compare this baseline with a bounded cache.

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

Storage reporting uses existing Pager and PageAllocator APIs. Results contain database
page count, file bytes, free-page count, and resident-frame count immediately before and
after the measured workload, plus human-readable file growth.

## Methodology

Use a Release build for representative measurements:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Each repetition removes the configured benchmark database, creates deterministic setup
data, optionally warms relevant hot reads, resets Pager counters, measures operations,
and validates state outside the timed region. The database is removed afterward unless
`--retain-db` is supplied. Insertion workloads deliberately time insertion into an
otherwise empty structure; lookup/update/delete/scan workloads populate first without
timing setup.

Every operation is timed independently using `std::chrono::steady_clock`. Reported
`total_ns` is the sum of those measured intervals; mean and throughput derive from that
sum. The timer-call overhead is therefore present, and nanosecond values should not be
interpreted as precision guarantees. No workload currently uses batch-derived latency.

Latency percentiles use nearest rank: sort `N` samples, compute
`rank = ceil(percent * N / 100)`, constrain rank to `1..N`, and return element
`rank - 1`. The tool reports count, total, operations/second, mean, p50, p95, p99,
minimum, and maximum in nanoseconds.

Fixed seeds make data generation, key order, and operation selection reproducible.
The default seed is `12345`. Tuple payload bytes and sizes are derived from the seed and
tuple index. The size modes are `small` (1–64 bytes), `medium` (129–512), `large`
(513–1500), and `mixed` (all ranges plus 17–128-byte tuples).

### Hot and reopen modes

`--mode hot` retains one owner/Pager. Relevant point-read workloads can perform
`--warmup N` untimed operations before counters reset. `--mode reopen` flushes through
normal owners where applicable, destroys and recreates the Pager/database owner every
`--reopen-interval N` measured operations, and aggregates counters across segments.

Reopen clears MiniDB++'s cache. It is **not** a cold-disk benchmark: the operating
system may retain file pages. The harness never drops OS caches or requires privileges.

`--working-set N` limits the key/page population sampled by supported lookup workloads;
zero means the whole available dataset. This is retained for future replacement-policy
comparisons.

## Workload definitions

The executable supports family aliases (`pager`, `bplus`, `tuple`, `sql`, `tcp`, and
`mixed`) and these named workloads:

| Family | Workloads and measured region |
| --- | --- |
| Pager | `pager_sequential`, `pager_random`, `pager_hot`: reads of pre-created pages; hot preloads its working set |
| Persistent B+ tree | `bplus_insert_sequential`, `bplus_insert_random`, `bplus_find_hit`, `bplus_find_miss`, `bplus_range`, `bplus_mixed` |
| TupleStore | `tuple_insert`, `tuple_lookup`, `tuple_update`, `tuple_erase`, `tuple_scan`, `tuple_fragmentation` |
| Local SQL | `sql_pk_lookup`, `sql_heap_scan`, `sql_insert`, `sql_update`, `sql_delete`, `sql_mixed`, `sql_pk_vs_heap` |
| TCP loopback | `tcp_pk_lookup`, `tcp_heap_scan`, `tcp_insert`, `tcp_mixed` |
| Mixed profiles | `mixed_read_heavy`, `mixed_write_heavy` (local SQL) |

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
socket transfer, server execution, and the server's current `Pager::flushAll()` after
each successful statement. They must not be compared to local SQL without accounting
for those semantics.

All workloads validate their relevant storage/tree/catalog/model after measurement.

## Suites and commands

Both `quick` and `baseline` contain:

```text
pager_sequential  pager_random  bplus_find_hit  tuple_lookup
sql_pk_lookup     sql_heap_scan sql_mixed       tcp_pk_lookup
```

`quick` caps rows at 32, operations/pages at 24, warmup at 8, and reopen cadence at 12.
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
```

See [the benchmark command reference](../benchmarks/README.md) for every option.

## JSON schema version 1

The output root is `{"schema_version":1,"results":[...]}`. Each result contains:

- `benchmark`, `seed`, and one-based `repetition`;
- `configuration`: rows, operations, pages, working set, warmup, reopen interval,
  repetitions, mode, and tuple sizes;
- `timing`: operation count, total, throughput, mean, p50/p95/p99, min/max;
- `pager`: all nine Pager statistics;
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

## Limitations and interpretation

- This harness is single-process and single-client; it is not a concurrency benchmark.
- Per-operation timer overhead matters for very cheap cache hits.
- Reopen mode cannot bypass the OS page cache.
- The unbounded cache can consume memory proportional to distinct pages accessed.
- First-fit storage behavior, server flush policy, validation, and current SQL features
  are intentionally measured as implemented, not optimized away.
- Git metadata is injected simply at CMake configure time and does not encode worktree
  cleanliness; reconfigure after changing commits for an exact identifier.
- No automatic cross-run comparison or timing-regression threshold is included.

The next storage milestone should keep these metric definitions stable where possible
and explicitly define buffer-pool requests, hits, misses, evictions, pins, dirty victims,
and resident-capacity behavior before making baseline comparisons.
