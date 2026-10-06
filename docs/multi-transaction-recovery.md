# Multi-transaction recovery substrate

Recovery represents independent WAL transactions and resumes multiple losers. **SQL
still admits exactly one write-capable transaction**, through the existing exclusive
database access lease. Concurrent readers remain supported; concurrent physical
writers are not. Transaction-safe overlapping allocation rollback and write-conflict
control are prerequisites for changing that boundary.

## Ownership and transaction state

`RecoveryCoordinator` owns a `TransactionId -> RecoveryTransactionContext` map. Each
context contains original before-images, cumulative delta masks, WAL accounting and
the value-only recovery entry: ID, status, BEGIN LSN, last LSN, current undo target,
start page count, CLR count, durable COMMIT/ABORT flags. Immutable snapshots copy
entries under the context mutex; they contain no frame pointers or sessions.

Keyed APIs are `beginTransaction()`, `beginMutation(id)`, `prepareTransaction(id)`,
`commitTransaction(id)` and `rollbackTransaction(id)`. SQL passes its session-owned
ID explicitly. The existing statement APIs are compatibility wrappers for startup,
tests and administrative callers. A non-owning binding routes physical write-intent
hooks; preparing an evicted page finds its owning context, not the last bound writer.
Binding cannot switch away from an unfinished physical mutation. Simultaneous page
ownership by different contexts is rejected. WAL mutation/admin APIs still require
external serialization; the context mutex does not enable parallel WAL appenders.

Normal transitions are `ACTIVE -> COMMITTED` or `ACTIVE -> ABORTING -> ABORTED`.
Analysis infers ABORTING from a CLR, rejects updates/COMMIT after rollback begins,
and accepts durable ABORT as terminal. Zero-write runtime contexts emit no WAL and
are absent from checkpoint ATT. READ ONLY sessions never create recovery contexts.

## Analysis and chains

Global WAL order and transaction-local order differ:

```text
LSN order: T1 BEGIN, T2 BEGIN, T1 U1, T2 U1, T1 U2, T2 COMMIT
T1 chain: BEGIN <- U1 <- U2                         (loser)
T2 chain: BEGIN <- U1 <- COMMIT                     (winner)
```

Every forward-analyzed `prevLSN` must equal that transaction's last accepted record
and precede the current record. This rejects cross-transaction links, missing or
nonboundary references, cycles, duplicate BEGIN and records after a terminal record.
CLR targets must identify the next uncompensated existing-page update of the same
transaction/page; `undoNextLSN` must equal that update's `prevLSN`. Skipping or
repeating existing-page compensation is corruption. Appended-page updates can be
skipped because legacy rollback removes them through final truncation.

`RecoveryManager::analysisTransactions()` exposes the classified value-only table;
`recoveredTransactions()` exposes terminal entries after UNDO. Winners' updates and
all durable CLRs remain REDO candidates. DPT/recLSN and persistent PageLSN filtering
are unchanged. This is not full ARIES repeat-history REDO of every loser update.

One recovery-local LSN index points into scan-owned decoded records. It is built
once, and UNDO never repeatedly scans the WAL. Space is O(R) for R scanned retained
records, plus payload storage and transaction/page state. `recoveryLsnIndexBytes`
accounts keys/pointers and buckets, **not** allocator overhead or decoded payloads;
there is no constant-memory or hard total-retained-WAL bound. Existing record length,
checksum and offset limits still apply. Analysis uses ordered transaction/page maps;
REDO candidate sorting is O(R log R). UNDO scheduling is O(U log L), with U visited
records and L losers, plus page/WAL I/O and synchronization.

## Live fuzzy ATT and retention

The existing version-1 **48-byte ATT entry is unchanged**: transaction ID, ACTIVE
status (1), BEGIN LSN, last LSN, startPageCount and reserved zeros. Only safe ACTIVE
contexts are published; rollback cannot overlap a checkpoint, so no new persisted
status values are needed. All WAL/page preparation must finish and page guards must
be released before marking a transaction's between-statements boundary safe.

Automatic fuzzy thresholds may publish at that boundary while an explicit writer
remains active. Administrative callers can use
`transactionManager().checkpoint(sessionId, CheckpointMode::Fuzzy)`, which locks the
session and borrows its existing exclusive lease. This adds no SQL or wire command.
The ordinary manual checkpoint API still rejects an active writer rather than
recursively acquiring its lease. Sharp checkpoints remain deferred/prohibited until
the writer finishes. No checkpoint is permitted inside a mutating statement or UNDO.

Fuzzy publication copies DPT and canonical ID-sorted ATT, appends/forces END, publishes
control and then rotates/reclaims. It does not commit the active writer or force
database pages. The floor is:

`min(checkpoint BEGIN, every DPT recLSN, every logged active transaction BEGIN)`

One predecessor segment remains retained as before. Clearing DPT by flushing pages
does not release ATT's BEGIN constraint. Completing the oldest transaction permits
advancement to the next actual constraint at a subsequent checkpoint.

Restart scans from the minimum DPT/ATT/checkpoint boundary. For ATT transactions it
reconstructs retained pre-checkpoint chains from BEGIN, checks exact snapshot
BEGIN/last/startPageCount/ACTIVE state at checkpoint BEGIN, then analyzes the tail
independently. Pre-checkpoint records of unrelated completed history retain the
existing conservative REDO behavior (their BEGIN may already be reclaimed).
Declared ATT IDs must be below checkpoint nextTransactionId. COMMIT after checkpoint
makes a winner; durable ABORT makes a finished rollback; otherwise the complete chain
remains a loser. Retained-WAL checkpoint discovery also works after control loss.
Recovered nextTransactionId exceeds all observed valid IDs and checkpoint state;
overflow is rejected, never wrapped into ID zero.

## Reverse-LSN UNDO

The priority queue holds one `(targetLSN, TransactionId, transaction-entry index)` per loser. It
selects the globally largest outstanding original/CLR target; unique LSNs make
ordering deterministic. Each transaction owns its CLR `prevLSN`, current undo target
and completion state. A CLR jumps to its `undoNextLSN`; an existing-page update emits
the unchanged full compensated-page CLR, forces WAL, installs CLR PageLSN and
writes/syncs the page. Newly appended pages retain the old truncation protocol.
Reaching BEGIN synchronizes and forces that transaction's ABORT independently.

Restart can therefore find finished losers, partially compensated losers and untouched
losers together. Durable CLR progress is REDO-able and not logically undone twice.
Live rollback uses the same scheduler, but uses its retained original page as delta
base: disk might lack a preceding NO-FORCE winner. Existing failpoints remain; tests
also use `MINIDB_RECOVERY_CRASH_AFTER_CLRS=N` after N newly forced/written compensations.
Production leaves test failpoint variables unset.

## Allocation and conflict restriction

Global `startPageCount` truncation is **not transaction-local allocation ownership**.
Multiple-loser histories are accepted only when unfinished transaction boundaries equal
the existing preallocated page count. Recovery rejects conflicting simultaneously
active physical page ownership and loser truncation that would remove a winner page.
Earlier completed allocating winners remain supported; their historical start counts
do not constrain later preallocated losers.
The primary synthetic histories use disjoint, preallocated pages, never shared page
0 or interleaved allocation. A single production writer retains legacy appended-page
rollback. No PageAllocator redesign, arbitrary same-page multi-writer recovery,
fine-grained locking, deadlock detection or MVCC is claimed.

Operations still lack general power-loss/torn-page/multi-file atomicity. Safe tested
process-crash boundaries and restartable CLR progress do not make arbitrary concurrent
allocation safe. Before multiple writers: design allocation ownership and durable
reclamation rollback, page/logical write-conflict control and deadlock handling,
WAL appender synchronization, isolation semantics, and a broader adversarial recovery
test matrix. Recovery generalization alone is not sufficient admission control.

## Tests and measurements

Recovery counters expose `analysisTransactions`, `analysisWinners`, `analysisLosers`,
`analysisAborted` and `peakRecoveryTransactionTableSize` (all classified entries retained
by analysis, not only currently active entries). `analysisActiveTransactions` and
`analysisAbortingTransactions` distinguish initial loser state. `multiLoserUndoSteps`,
`multiLoserClrsAppended` and `transactionsCompletedDuringUndo` describe restart work;
`undoQueuePeak` measures simultaneously scheduled losers. `recoveryLsnIndexEntries`
and `recoveryLsnIndexBytes` provide the accounting described above. Existing single-
loser summary fields remain compatibility diagnostics, not a complete multi-loser view.
Checkpoint counters add cumulative `attEntriesCaptured`, most recent
`oldestActiveTransactionBeginLsn` and `activeTransactionRetentionFloor`; an empty ATT
uses INVALID_LSN, not a misleading zero-entry floor.

`multi_transaction_recovery_test` uses real codecs, disk pages and restart logic:
51,200 cumulative byte-range updates, four seeds (0x12B201, 0xC0FFEE, 0xA11CE, 0xDE1A),
eight histories per seed, 16 transactions per history, 8 winners/4 aborted/4 losers.
Even histories use segmented WAL and nonempty checkpoint ATT; odd histories use
legacy single-file WAL. Optional STEAL writes, PageLSN/bytes, terminal lastLSN, exact
classification, next ID and idempotent second recovery are checked. Directed tests
cover 1/2/4/8/16 losers, repeated crashes, independent durable ABORT amid partial and
untouched losers, corrupt chains/ATT, runtime contexts/retention and six SQL subprocess
crash cases (winner/rollback/loser, with/without checkpoint control).

Build/run `multi_recovery_benchmark` for real-WAL analysis at 1/10/100/1000 transactions,
128 total UNDO updates across 1/2/4/8/16 losers, 8-loser crashes after 25/50/75% progress,
and a long-lived ATT spanning 10 fuzzy checkpoints plus 1000 newer commits. It reports
timings, index accounting, queue operations, writes and retention directly; timings
are machine-specific and have no pass/fail thresholds. See [benchmarking.md](benchmarking.md).
