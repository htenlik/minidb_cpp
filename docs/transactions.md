# Explicit serial transactions

MiniDB++ supports `BEGIN [TRANSACTION]`, `COMMIT`, and `ROLLBACK`, case-insensitively,
with an optional semicolon. Send one statement per request on the same TCP connection:

```sql
BEGIN;
INSERT INTO accounts VALUES (1, 100);
UPDATE accounts SET balance = 150 WHERE id = 1;
SELECT * FROM accounts WHERE id = 1;
COMMIT;
```

By default, each mutating statement remains an implicit atomic transaction. An explicit
transaction keeps one TransactionId, original-page set, BEGIN page-count boundary, and
WAL `prevLSN` chain across all its statements. Successful intermediate statements do
not commit. SELECT observes the connection's own changes.

## Lifecycle and ownership

`TransactionManager`, owned by `SqlEngine`, keeps `Idle` or `Active` plus the owning
SessionId. `RecoveryCoordinator` holds physical recovery state for that complete scope.
Storage, indexes, and the parser do not manage transaction lifetime. The server executes
serially and permits only one explicit transaction globally. A different session cannot
read, mutate, commit, or roll back the owner's active scope. TCP connections receive
monotonic process-local session IDs; embedded calls default to session zero.

Nested BEGIN, COMMIT while idle, and ROLLBACK while idle return execution errors.
There are no savepoints, multiple active transactions, locks, MVCC, or isolation levels.
Do not copy a transaction manager or use multiple independent managers against the same
storage/recovery objects.

## WAL and completion

BEGIN allocates an in-memory transaction ID and captures the current page count before
any allocation. With serial ownership that is also the pre-first-mutation boundary.
The WAL BEGIN is materialized only by the first logged change. Each successful mutating
statement prepares the latest resident touched pages without committing or forcing them.
Eviction may prepare and force updates sooner under STEAL.

Read-only BEGIN/SELECT/COMMIT and BEGIN/SELECT/ROLLBACK generate no transaction WAL.
Their in-memory IDs increase during the process; an ID never persisted in WAL or a
checkpoint can be reused after restart. Logged transaction IDs remain monotonic across
reopen. Avoiding that ephemeral-ID reuse would require durable reservation even for
read-only transactions.

COMMIT prepares remaining page states, appends one COMMIT, and forces WAL through it.
Database pages are not forced by COMMIT. A separately triggered pending sharp checkpoint
can flush them after commit. A crash after COMMIT fsync but before the response leaves
the transaction committed with an ambiguous client outcome; request IDs are not retry
deduplication tokens.

Crash tests use process termination, not a power-loss simulator. The after-append
failpoint tests a COMMIT still in the userspace WAL buffer, which is lost on exit.
Recovery follows the valid surviving log; an unacknowledged commit can also survive
if its bytes reached the filesystem before an interrupted force completed.

ROLLBACK uses the same CLR traversal as startup loser UNDO. It prepares and forces
pending transaction WAL, invalidates touched buffers, follows the transaction chain,
forces a full-page CLR before each compensation write, syncs pages, repeats safe
truncate-to-BEGIN-page-count cleanup, and forces ABORT before returning success. A crash
during rollback resumes via CLR `undoNextLSN`. Delta compensation during live rollback
uses the retained original page as its base: an earlier committed NO-FORCE winner might
still be absent from disk. No unrelated winner history is replayed over live buffers.

## Errors, disconnect, and DDL

Parser/lexer errors occur before execution and leave an explicit transaction active.
SELECT errors also leave it active. Any error executing a mutating statement triggers
full durable rollback of the explicit scope, including earlier successful statements,
then returns to Idle. There is no statement savepoint or persistent SQL failed state.
An I/O/completion failure makes the engine unavailable until reopen; it cannot safely
continue from uncertain COMMIT or partial CLR progress.

EOF, protocol failures, and broken TCP connections roll back their owned scope before
the serial server accepts subsequent work. `DatabaseServer::close()` also rolls back
durably and reports failures; destruction attempts the same cleanup. A hard process
crash instead leaves startup recovery to finish the loser. Shutdown never commits.

`CREATE TABLE` is permitted inside a transaction. Its catalog entries, metadata, heap,
index, and allocator changes use existing recovery hooks; rollback removes the created
table and restores the page-count/free-list state. This does not imply support for
unimplemented ALTER, DROP, or other DDL.

## Checkpoints and reclamation

Manual sharp and fuzzy checkpoints reject an active scope, including read-only scopes.
Automatic WAL-byte and successful-mutation-statement thresholds set `pending()` while
active. COMMIT, ROLLBACK, error rollback, and disconnect evaluate that pending request
at the next safe boundary. Failed automatic checkpoints remain pending for retry.
Statements later rolled back still count toward this work-based threshold.

No production checkpoint overlaps an active transaction; its ATT remains empty. WAL
reclamation therefore cannot remove an active chain's BEGIN or remaining UNDO targets.
Normal checkpoint/reclamation resumes after durable COMMIT or ABORT.

## Diagnostics and memory

`transactionManager()` exposes read-only state/identity, `hasMaterializedWalBegin()`,
`lastLsn()`, `touchedPageCount()`, `transactionWalBytes()`, and cumulative counters.
Counters distinguish explicit begin/commit/rollback, error/disconnect/shutdown rollback,
implicit commits, successful statements within explicit scopes (excluding control),
completed zero-log explicit scopes, and encoded transaction WAL bytes including CLR and
terminal records. Rolled-back explicit transactions include all rollback reasons.

Original page before-images remain resident until the transaction ends. The original
bytes/pages metrics count existing-page images; peak recovery bytes account for the
context plus map entry values, including before/latest images and touched-byte masks.
They exclude map allocator overhead, temporary encoding buffers, and global metric
sample vectors, so they are not process RSS. Long transactions can consume significant
memory even with a bounded buffer pool.

Statement preparation scans all touched pages (`O(P)` plus page comparisons), and full
physical rollback visits the transaction's logged updates (`O(U)` page work and syncs).
Full-page CLRs cost 4,184 encoded bytes each. Benchmarks intentionally report that WAL,
memory, and force cost; physical logging remains unchanged.

## Protocol compatibility

Transactions travel as ordinary EXECUTE_SQL requests with ordinary command results,
zero affected rows, and no fabricated result set. Existing v1 framing, field offsets,
and request semantics are unchanged. Command-kind IDs 5/6/7 add BEGIN/COMMIT/ROLLBACK;
old clients that reject unknown command IDs need an update to consume these responses.

See [recovery.md](recovery.md), [clr-restartable-undo.md](clr-restartable-undo.md),
[checkpoints.md](checkpoints.md), and [benchmarking.md](benchmarking.md).
