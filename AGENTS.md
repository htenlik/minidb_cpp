# Development rules

Keep SQL/execution, Catalog/Table, heap/index, allocator, buffer pool, disk, and WAL
responsibilities separate; see [architecture](docs/storage-architecture.md).
Production permits concurrent readers and **one exclusive writer**, not multi-writer
allocation; preserve the [concurrency boundary](docs/concurrency-baseline.md).

Preserve WAL-before-data, transaction chains, restartable CLR UNDO, persistent
PageLSN identity, dirty recLSN/DPT and live ATT retention. Checkpoint authority
requires the established durability/publication ordering. Follow
[recovery](docs/recovery.md) and [frame retirement](docs/frame-wal-dependency-retirement.md).
Preserve all legacy database/WAL formats and compatibility tests.

Use incremental builds and FAST → FOCUSED → FULL → RELEASE as defined in
[the workflow](docs/development-workflow.md). Never disable failing correctness
tests, reduce seeds, or trim crash coverage. Inspect diffs before commits; do not
merge before applicable verification passes. Preserve release tags; never force-push.

Do not alter or merge these experimental branches without explicit approval:

- `feature/checkpoint-safe-frame-lsn`
- `feature/nontruncating-transaction-rollback`
- `feature/transaction-owned-allocation`

The portfolio-focused freeze plan is isolated prototype reevaluation, final
correctness review, behavior-preserving cleanup, then release. New features or
owned-rollback activation require separate authorization.
