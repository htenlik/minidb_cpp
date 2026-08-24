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
