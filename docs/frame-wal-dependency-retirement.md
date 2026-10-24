# Clean frame WAL dependency retirement

Page history and a buffer's current WAL-before-data obligation have different
lifetimes. Keeping every historical PageLSN in WAL forever is unnecessary;
ignoring missing WAL for dirty pages is unsafe.

## Identities and states

- **Persistent PageLSN** lives in a recognized page type's existing bytes. It
  identifies the last applicable update/CLR/reclaim, even after a safe checkpoint
  makes that WAL obsolete. Retirement never edits it.
- **Historical frame `pageLsn`** is the latest logged identity assigned while
  resident. Guards keep exposing it after retirement. Installation starts at
  `INVALID_LSN`, as before; persistent bytes are not cleared or decoded using
  an unrelated type's layout.
- **Live dependency** requires that historical LSN still identify retained WAL.
  It protects dirty changes and clean writebacks not yet checkpoint-covered.
- **`recLsn`** is the first logged modification in the current dirty period, not
  a historical identity. Writeback clears it; repeated dirty updates preserve it.

Two volatile flags accompany the existing fields:

| Frame state | dirty | pendingWalPreparation | walDependencyRetired | recLSN |
| --- | --- | --- | --- | --- |
| Installed clean, no volatile LSN | false | false | false | invalid |
| Unprepared write intent | true | true | prior value | prior dirty period, or invalid |
| Logged dirty | true | false | false | valid, retained, <= pageLSN |
| Written, not checkpoint-covered | false | false | false | invalid |
| Checkpoint-covered historical | false | false | true | invalid |

Retirement applies only to the historical identity. A subsequent pending guard
is **not** prepared for persistence. A real update establishes a fresh live
dependency and the first recLSN of a new dirty period. Preparation can instead
prove an intent was a no-op: clean history stays clean, or a previous still-dirty
logged period keeps its original recLSN. Clean touched pages are not re-prepared.
Educational pools without a WAL provider retain their unlogged behavior.

## Authorization and proof

`BufferPoolManager::retireCheckpointCoveredDependencies()` is private to the
checkpoint manager (plus surgical test access). It is not a public instruction
to ignore an unknown LSN. Its sole production caller runs under existing exclusive
checkpoint authority at a prepared between-statements boundary:

```text
sharp: flush dirty frames -> database fsync
fuzzy: syncPendingWritebacks(), WITHOUT flushing still-dirty frames
    -> append/force checkpoint WAL
    -> publish/fsync authoritative control
    -> preflight + retire eligible clean dependencies
    -> rotate/reclaim obsolete segments
```

Reconciliation runs **before** best-effort reclamation rather than after it.
Successful, failed and partially completed deletion attempts therefore all leave
eligible frames classified correctly. Preflight still has live WAL available to
validate, rather than deriving permission from `containsLsn=false`. Retirement
does not change global retention floors.

Under the buffer metadata latch, preflight validates identity, page table,
replacer/pins, all live PageLSNs/recLSNs, and rejects pending preparation.
DiskManager's existing snapshot must have `initialSyncRequired=false`,
`writeFailure=false`, and equal physical/synchronized epochs. Exclusive checkpoint
authority prevents intervening writers, so this whole-file frontier covers all
clean writebacks. Newly retired LSNs must also be WAL-durable. Only clean valid
frames with invalid recLSN and a known historical identity retire. Dirty frames
stay live with unchanged DPT entries, even if their transaction committed.

An active transaction can own a written clean frame. Its frame-local dependency
may retire while ATT retains BEGIN, updates and UNDO chains. Failed control
publication never runs retirement. Failure after publication but before retirement
occurs before any reclamation; retained history remains available on retry/restart.

## Writes, eviction and locking

Every WAL-backed guard write intent/mutable-data access becomes pending. Dirty
flush/eviction must prepare it; without a preparer, explicit `setPageLsn` must
supply its logged identity. DPT snapshots reject pending images. Recovery's
safe-boundary check is unchanged.

Coordinator preparation and explicit assignment both validate known records and
reject LSN decreases against preserved history. Page-type transitions keep using
existing V2/CLR normalization, not a new generic persistent offset. Single/victim
flushes validate the live dependency and retained recLSN before WAL force/write.
Batch flush preflights **every** dirty dependency, then coalesces forcing to one
maximum-LSN request. A missing smaller LSN cannot hide behind a valid maximum.
A clean retired victim requires no historical WAL force or page write. Frame
installation/discard resets both flags, preserving identity/pin/replacer rules.

Checkpoint releases disk I/O locking before entering buffer metadata. Retirement
takes metadata then reads the disk snapshot, like existing buffer I/O; it never
waits for content latches or changes page bytes. Read guards remain valid. Raw
callers without the database gate must provide equivalent exclusion. No new
concurrent-writer guarantee is introduced.

## Failure, restart and cost

Retirement emits no page write, extension, WAL record or extra database fsync.
Existing sharp sync or fuzzy conditional synchronization supplies durability.
The scan is O(buffer capacity), with O(capacity) temporary validation storage.
There is no retirement sidecar: restart uses existing database, checkpoint,
DPT/ATT and WAL facts; volatile flags disappear.

Before repair, `reclaim_lineage_test --frame-diagnostic` found P30 resident/clean,
recLSN invalid, volatile/persistent LSN both 310. Four sharp checkpoints deleted
two segments; record 310 disappeared, pending physical writes were zero, and
validation threw `Buffer frame pageLSN is unknown to its WAL provider`. The same
logical history, deletion and bytes now pass, independently registered in CTest.
The preserved unmerged audit branches remain unchanged.

Counters distinguish this operation from deletion and synchronization:
`cleanDependenciesRetired` counts transitions; `dirtyDependenciesPreserved` counts
dirty frames encountered per reconciliation; `dependenciesRenewedAfterRetirement`
counts fresh assignments; `retirementReconciliationCalls`/`Failures` count attempts.
`retirementDurationNs` measures successful preflight/transition after acquiring
metadata. `historicalMissingWalAccepted` and `liveMissingWalRejected` count
validation observations, not unique records/pages.

## Verification and limits

Directed tests cover all three WAL modes, sharp/fuzzy, real segment unlink,
unchanged bytes, mixed dirty/clean buffers, fresh writes/rollback, no-op intent,
monotonicity, unprepared manual writes, unavailable latest/recLSN, batch maximum
hazards, failed synchronization/control, active ATT, repeated Free/Slotted/CLR
reuse, partial reclamation exceptions, deleted control and latest-slot fallback.
Existing lineage tests additionally cover opaque and B+ leaf/internal transitions.

The new crash matrix is **20 boundaries x 3 WAL modes x 2 REDO policies = 120**
hard subprocess exits: database fsync, fuzzy BEGIN/END/force, control publication,
both sides of retirement, completed rotation, manifest/unlink, fresh update
append/force/writeback, COMMIT append/fsync, interrupted CLR UNDO. Each restores
the last successful fsync shadow before real recovery. Existing 84-prefix frontier
and 72-case resident/evicted/dirty/page-0 loss matrices remain unchanged.
Six additional hard-exit/shadow-loss cases exercise direct page-0 catalog/free-list
root fields together and compare the entire recovered metadata page, including PageLSN.

The model completes **30,000** operations: 7,500 per seed `0xF2A1`, `0x51A7E`,
`0xC1EA4`, `0x5EED9`, across **120** hard restarts (60 process-only, 60 shadow-loss).
It uses real WAL/page operations, three update modes, both REDO policies, buffer
pressure, writes/no-ops, rollback/CLR, writeback, sync, sharp/fuzzy publication,
rotation and physical deletion. Checks cover exact bytes, persistent/historical
LSNs, live/retired/pending classification, dirty periods, DPT/ATT, retained history,
checkpoint authority and physical write epochs. Reuse additionally has 36 directed
retire/reclaim/CLR cycles and existing models. Failed prefixes are not completion.

Separate TSan tests cover historical readers/checkpoints plus existing buffer,
session and TCP paths. [Benchmarks](benchmarking.md#frame-dependency-retirement-experiment)
compare identical prior-baseline workloads. Shadow loss models unsynchronized
writes, **not** hardware power cuts, torn pages or arbitrary external file writers.
[Production owned rollback](owned-rollback-activation.md) is active under the same
single-writer boundary. Shared-allocator publication and multiple production writers
remain unsupported; frame retirement supplies neither capability.
