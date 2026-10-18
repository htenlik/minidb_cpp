# Physical writeback and database synchronization

MiniDB++ distinguishes stream writeback from successful database-file `fsync`.
`fstream::write`/`flush` can make a page visible to another reader without proving
that its bytes survive loss of unsynchronized storage writes. A clean buffer
frame means that no buffered modification still needs writeback; it does not,
by itself, certify stable database storage.

## DiskManager's volatile frontier

`PhysicalWritebackStats` contains:

| Field | Meaning |
| --- | --- |
| `physicalWriteEpoch` | Number of accepted database mutation operations in this DiskManager instance |
| `synchronizedWriteEpoch` | Highest write epoch covered by a successful database fsync |
| `initialSyncRequired` | Opening a file does not establish its prior synchronization state |
| `writeFailure` | A failed/possibly partial write poisons further mutations and checkpoint synchronization until restart |
| `databaseSyncsCompleted` | Successful sync operations; no-op conditional checks are not counted |
| `databaseSyncFailures` | Sync operations that failed before publishing a synchronized frontier |

The epochs are `uint64_t`, protected by the existing recursive I/O mutex. They are
not page generations, allocation ownership or persistent state. No page, WAL or
checkpoint bytes change. Exhaustion is checked **before** attempting a mutation;
the write epoch advances only after the operation is accepted. An uncertain failed
write does not advance the epoch or become falsely synchronized: it poisons the
instance instead. The counter measures accepted mutation operations, not individual
kernel `write` syscalls or individual changed bytes.

An existing file opens at epochs 0/0 with `initialSyncRequired=true`, rather than
inventing a write or assuming that a previous process fsynced. Production startup
recovery synchronizes the database before normal use and clears this uncertainty.
Creating an empty database counts its initial metadata-page write.

## Physical path audit

| Path | Accounting |
| --- | --- |
| `writePage` | One page-image write and stream flush, one epoch |
| `writePhysicalPage` | One image write, one epoch; each required zero-page extension is a separate tracked append |
| `appendPage` (both overloads) | One accepted zero-page append, one epoch; checked expected PageId and existing WAL reservation ordering are unchanged |
| `persistDatabaseHeader` | Uses the same tracked page-byte helper; one epoch, including initialization |
| Catalog/free-list root updates | Header persistence above; no extra/double increment |
| `DatabaseMetadataManager::persist` | WAL-before-data, direct physical page-0 write through DiskManager; covered despite not being in buffer DPT |
| Buffer flush/dirty eviction | Uses `writePage`; tracked even after the frame is evicted |
| Legacy Pager | Uses this DiskManager's append/write/header methods; no separate untracked database writer |
| Recovery/CLR REDO/UNDO | Physical writes and extent changes use the same tracker; existing explicit database sync establishes its frontier |
| `truncateToPageCount` | One accepted extent mutation, one epoch; identical extent is a no-op; failure poisons the instance |
| Restricted PAGE_RECLAIM | Its Free Page and page-0 writes are tracked separately; its existing final database sync covers both |
| `flush` | Stream flush only; does not advance synchronized epoch |
| WAL/control/shadow files | Not database-file mutations; their separate synchronization protocols remain unchanged |

All production database-file writes converge in DiskManager. Raw setup writes are
also tracked but do not acquire transaction semantics. External file writes or
independent writers to the same path remain unsupported. Creating a path is not a
new directory-durability protocol; no new filesystem/hardware guarantees are claimed.

## Atomic conditional synchronization

`syncPendingWritebacks()` returns false when no work is pending, true only after
an actual successful database fsync, and throws on failure. The check, stream
flush, fsync and frontier publication all hold the same I/O mutex. Therefore a
concurrent later write cannot be attributed to an earlier fsync: it waits, then
advances the write epoch and becomes pending again.

`sync()` remains unconditional for sharp checkpoints, recovery and synchronous
system operations. On success it sets synchronized epoch to write epoch and clears
initial-open uncertainty **after** fsync returns successfully. On fsync error the
frontier stays unchanged; pending state survives and a later retry can succeed.
Failed physical writes instead require restart. Reads do not clear that poison.

The private syscall seam used by tests injects a `-1`/`errno=EIO` fsync result for
failure cases; it is not a user-facing storage or checkpoint option. Successful
observed calls and production both use POSIX `fsync`.

## Fuzzy checkpoint publication

```text
exclusive checkpoint authority + prepared between-statements boundary
    -> conditional database sync of already-accepted physical writes
    -> FUZZY_BEGIN
    -> snapshot still-dirty DPT and active ATT
    -> FUZZY_END
    -> force checkpoint WAL through END
    -> publish/fsync checkpoint control
    -> rotate WAL and reclaim below DPT/ATT retention floor
```

The fence runs before checkpoint WAL/control publication. Failed synchronization
does not append the new fuzzy BEGIN, advance control generation or reclaim WAL.
A checkpoint ID may be skipped on retry; that is not checkpoint authority. An
fsync that succeeded before a later publication failure remains a real completed
fsync and is counted as such.

The fence does **not** call buffer `flushAll`, acquire frame content latches,
change dirty state/recLSN, assign persistent PageLSNs or retire volatile PageLSNs.
Resident dirty pages remain in the ordinary DPT, including pinned read-guard
pages. Their WAL and live transaction BEGIN/UNDO chains remain protected by the
unchanged retention floor. Direct page 0 and evicted written pages are covered by
the synchronized single-file state, so no DPT format extension is necessary.

The production database exclusive lease prevents conflicting physical writers
between this fence and publication. A test-only caller without the access gate
must provide equivalent exclusivity; atomic conditional sync alone does not make
an entire checkpoint safe against arbitrary raw concurrent writes. There is no
new disk-to-buffer/WAL lock acquisition: checkpoint takes disk I/O directly, not
through buffer metadata. The syscall seam never runs in production.

Sharp checkpoints still flush all dirty frames and perform exactly their existing
one database sync. They need no additional conditional fence. PAGE_RECLAIM and
restartable CLR operations retain their existing force/write/sync ordering.

## Diagnostics and cost

Disk diagnostics are lifetime counters/snapshots, not resettable durability state.
Checkpoint `databaseSyncs` includes completed sharp and conditional fuzzy syncs.
`fuzzyCheckpointDurabilitySyncs` counts successful conditional syncs, and
`fuzzyCheckpointsWithoutPhysicalSync` counts no-op fence outcomes. These fence
counters can advance even if subsequent WAL/control publication fails; they do
not claim successful checkpoint publication.

Some fuzzy checkpoints now pay for database fsync. A no-pending checkpoint does
not. Many physical writebacks are covered by one fence, not one fsync per page.
See [benchmarking](benchmarking.md#physical-writeback-frontier-experiment) for
controlled observations; unchanged fuzzy-checkpoint performance is not claimed.

## Tests and crash model

`physical_writeback_test` covers exact epoch changes, page-0 metadata, eviction,
resident clean writeback, dirty-only DPT, batching, no-op fences, actual EIO
injection/retry, overflow before mutation, poisoned write failure, I/O serialization,
and the unchanged PAGE_RECLAIM sync boundary. Its 72 lost-write/reopen combinations
cover all three update modes, both WAL stores, selective/AlwaysRedo, committed/live
loser transactions, and resident/evicted/still-dirty page placement.

A test-only stable shadow copies database bytes **at each successful real fsync**
while the I/O mutex remains held. After subprocess hard exit the test can either
leave kernel-visible bytes intact (process crash) or restore that exact last
synchronized shadow (modeled loss of all later database writes). WAL/control files
retain their existing durable ordering. Restoring an older image after a new
successful fence would incorrectly discard synchronized writes and is not a valid
lost-unsynchronized-write model.

The original page-30 reclaim/reuse scenario retains its exact committed tuple
assertion and real segment deletion; it now survives this fsync-aware loss model.
The preserved audit branch is unchanged. No hardware power-cut, torn-page atomicity,
storage-controller cache behavior or directory-creation durability is proven.

The 84 hard-crash prefixes surround the database fsync call (including syscall return
before frontier publication), fuzzy BEGIN/END, checkpoint WAL force, control
publication and manifest/unlink reclamation. Each starts with generation 4 already
authoritative, and checks whether restart selects generation 4 or 5; segmented
cases exercise prior real reclamation before the interrupted checkpoint. They are
not claims of deterministic
termination inside a kernel fsync implementation. The 24,000-operation model uses
four fixed seeds (`0xF51C`, `0xC0FFEE`, `0xD0AB1E`, `0xE71C7`) and 96
hard-crash/reopen batches, alternating process-only and
lost-unsynchronized-write restarts. Each completed batch verifies exact committed
tuple bytes, free-list root/membership, epoch accounting, DPT/ATT snapshots and
actual WAL deletion. Failed prefixes are not counted as completion.

## Deliberately separate remaining work

The [clean-frame volatile PageLSN problem](buffer-wal-retention.md) is **not fixed**.
`reclaim_lineage_test --frame-diagnostic` may still fail after legitimate segment
reclamation; it is not a registered passing assertion for this change. A later
repair can use a proven synchronization/checkpoint boundary for explicit volatile
dependency retirement without clearing persistent PageLSNs or weakening dirty WAL
validation. Owned rollback, shared allocator ownership and multiple production
writers remain out of scope.
