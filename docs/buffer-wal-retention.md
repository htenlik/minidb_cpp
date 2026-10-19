# Buffer PageLSNs and retained WAL

Four LSN concepts have different lifetimes:

- **Persistent PageLSN** is historical metadata in the page bytes. A safe checkpoint
  can make its originating WAL unnecessary without changing those bytes.
- **Volatile frame pageLSN** preserves the most recent logged identity assigned
  while resident, including after checkpoint coverage.
- **Live dependency** separately requires retained WAL for dirty changes or a
  writeback that has not reached an authoritative checkpoint boundary.
- **recLSN** is the first update of the current dirty period; later updates preserve
  it, and successful writeback clears it. Writeback is not database fsync.

[Physical writeback tracking](physical-writeback-durability.md) now distinguishes
already-written unsynchronized state from synchronized database state. Fuzzy
checkpoints conditionally fsync those physical writes before publication, while
still-dirty frames retain their ordinary DPT entries and WAL dependencies.

## Checkpoint-authorized retirement

After durable checkpoint-control publication, the checkpoint manager
[retires eligible clean dependencies](frame-wal-dependency-retirement.md) before
best-effort segment deletion. Historical frame and persistent PageLSNs remain
unchanged. Missing WAL is accepted only for this authorized historical state,
not as a reason to infer retirement. Dirty latest LSNs and recLSNs remain strict.

The original independent reproducer is now a registered green regression:

```sh
./build/reclaim_lineage_test --frame-diagnostic
```

New writes become pending, then establish fresh live dependencies before physical
persistence. Retirement neither releases ATT/UNDO history nor changes global WAL
retention floors. The original unmerged audit branches and their deliberately
red tests remain unchanged.
