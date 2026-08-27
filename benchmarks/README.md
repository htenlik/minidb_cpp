# MiniDB++ benchmark command reference

Build in Release mode for representative results:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/minidb_bench --help
```

Select exactly one of `--benchmark NAME` and `--suite quick|baseline`. Options are:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--rows N` | 1000 | Setup rows for storage/SQL/index reads |
| `--operations N` | 1000 | Measured operations |
| `--pages N` | 1000 | Pager dataset pages |
| `--working-set N` | 0 | Sampled lookup set; zero is the full dataset |
| `--warmup N` | 100 | Untimed hot-mode warmup operations |
| `--mode hot|reopen` | hot | Retain owner or recreate it periodically |
| `--reopen-interval N` | 250 | Operations per reopen segment |
| `--buffer-frames N` | 64 | Bounded capacity for buffer and active engine workloads |
| `--lru-k N` | 2 | LRU-K history length for buffer and active engine workloads (`K >= 1`) |
| `--wal-payload-bytes N` | 128 | Opaque bytes per standalone WAL record |
| `--wal-batch-size N` | 10 | Records between forces in `wal_batch_flush` |
| `--wal-buffer-bytes N` | 65536 | LogManager memory-buffer capacity |
| `--wal-segment-bytes N` | 16777216 | Fixed segmented-WAL payload capacity |
| `--wal-update-mode MODE` | `full-page` | `full-page`, `byte-range`, or `adaptive` page updates |
| `--checkpoint-wal-bytes N` | 67108864 | Automatic checkpoint growth threshold; zero disables |
| `--checkpoint-statements N` | 0 | Automatic checkpoint commit threshold; zero disables |
| `--tuple-sizes MODE` | mixed | `small`, `medium`, `large`, or `mixed` |
| `--seed N` | 12345 | Deterministic workload seed |
| `--repetitions N` | 1 | Fresh-database repetitions per workload |
| `--db PATH` | `minidb_benchmark.db` | Controlled generated database |
| `--json PATH` / `--output PATH` | none | Machine-readable schema-v1 result |
| `--retain-db` | off | Keep the generated database afterward |

All positive counts are validated and capped at ten million; repetitions are capped at
100. Benchmark and database result artifacts are ignored by Git. See
[docs/benchmarking.md](../docs/benchmarking.md) for definitions, measured regions,
workload distributions, output schema, and interpretation caveats.

JSON identifies each result with `storage_backend`: `buffer_pool` for active B+ tree,
TupleStore, SQL, TCP, mixed, and standalone buffer workloads; `legacy_pager` only for
the historical low-level Pager family; and `wal` for the standalone logging substrate.
The WAL workloads are `wal_append_buffered`, `wal_append_flush_each`, and
`wal_batch_flush`; they report records/payload throughput, append/force latency, encoded
bytes, buffer drains, writes, and fsyncs. The same configuration object records frame/K
and WAL payload/batch/buffer controls for controlled comparisons.

Recovery-enabled workloads are `txn_insert`, `txn_update`, `txn_varchar_update`,
`txn_delete`, `txn_bplus_insert`, `txn_mixed`, `recovery_full_scan`, `recovery_loser`,
`checkpoint_latency`, `recovery_checkpoint_compare`,
`wal_segment_rotation`, and `wal_reclamation`.
Transaction results include per-encoding record counts, observed byte changes,
payload/total WAL amplification, record-size/range distributions, diff CPU time, and
fsyncs. Checkpoint results include dirty writes, required syncs, and latency; the
comparison reports full-history versus checkpoint-tail records/bytes/time. Configure
automatic-policy metadata with `--checkpoint-wal-bytes` and `--checkpoint-statements`
(zero disables). These are deterministic baselines, not timing-gated CI assertions.
