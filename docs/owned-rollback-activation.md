# Production owned rollback

New SQL READ WRITE scopes, including implicit mutations, use `BEGIN_OWNED`.
READ ONLY has no WAL context; a READ WRITE scope with no logged mutation retains
zero-write COMMIT/ROLLBACK semantics. There is no feature flag or new SQL syntax.
Production still admits concurrent readers and one exclusive writer.

## Runtime authority

TransactionManager first installs the real exclusive lease at its stable session
address. A private RecoveryCoordinator bridge binds that manager identity,
SessionId, TransactionId, gate and lease address to the new owned context.
`requireWriter()` checks that exact binding before mutation/completion; recovery
also checks the live lease and transaction identity at physical mutation/WAL
boundaries. The bridge cannot be selected through a public constructor, SQL,
network request or environment variable. Existing test friends are not called by
production and issue no production certificate.

The session keeps its lease uninterrupted across statements and final cleanup.
Only after the context has completed COMMIT or durable ABORT can the private
completion bridge clear the binding and the manager release the lease. A
completion failure poisons the engine, shuts admission, retains the writer lease
and requires reopen; disconnect/shutdown cannot acknowledge unfinished cleanup.
Gate shutdown cancels admission but does not revoke existing cleanup authority.

Lock order is unchanged: session latch, short gate admission, recovery context
mutex, then existing page/WAL/disk operations. The gate mutex is not held during
I/O; authority checks inspect the held lease rather than reacquiring the gate.
No buffer metadata latch is newly held across gate acquisition.

## Startup and compatibility

DatabaseServer invokes private `RecoveryManager::recoverAtStartup()` before a
buffer pool, SQL engine or listener exists. This constructor boundary acquires
a real exclusive startup lease and retains it throughout recovery; construction
failure admits no sessions. Its lifetime is distinct from later session leases.
As before, independent server instances/allocators mutating the same file are not
supported; this is not a new file-locking or multi-writer protocol.

The durable BEGIN type selects the algorithm, not the presence of a feature
toggle. Type 1 uses the original CLR/truncation path. Type 15 uses existing-page
CLR UNDO followed by validated APPENDED cleanup and final ABORT. A startup lease
authorizes execution, never substitutes for WAL reservation/materialization proof.
Raw RecoveryManager callers may replay valid committed owned histories; initiating
or resuming owned cleanup still requires the private startup/test authority.

Migration, initial Catalog creation and other direct pre-session coordinator
operations intentionally remain legacy. They finish before SQL admission.
There are no database, WAL payload, checkpoint, PageLSN or wire-format changes.
Legacy delta-REDO zero-base regression and both REDO policies remain covered.

## Physical cleanup

The [existing protocol](owned-rollback-protocol.md) is reused unchanged:
regular user CLRs and data sync; forced cleanup marker; exact reservation/image
proof; forced intent and associated PAGE_RECLAIM; Free Page/page-0 writes and
data sync; forced progress; final data sync and forced ABORT. PageReclaimer's
independent idle/certificate restrictions are not weakened.

Owned rollback never truncates its historical page-count suffix. It invalidates
touched/reserved frames only, preserves unrelated committed pages, restores
preexisting reused pages through CLR UNDO, and reclaims proven appended pages
exactly once. Absent reservations never fabricate pages. Physical extent can
remain at a reusable high-water mark. Page-0 user compensation precedes cleanup
head publication; checkpoints cannot advance over poisoned/incomplete cleanup.

Unlogged administrative mutation/byte-identical ABA, overlapping allocators,
multiple SQL writers, allocation generations, deferred free-list publication,
fine-grained locks and MVCC remain unsupported. No such mechanisms are added.

## Verification

`production_owned_rollback_test` uses ordinary public DatabaseServer/SqlEngine,
without the owned test fixture. It checks actual BEGIN_OWNED emission, one chain
per multi-statement scope, zero-write WAL, structural/relocation rollback, exact
rows/RIDs/free membership, cleanup-failure admission poisoning and default reopen.
Its crash target interrupts nine cleanup boundaries in each WAL mode and also
interrupts default startup intent-tail completion. Existing TCP/concurrency,
Stage-2/3 models and legacy raw-coordinator tests remain required.

SQL-specific old truncation assertions now require retained extent plus exact
reusable membership. Raw legacy truncation coverage remains unchanged. Final
Release, sanitizers, fresh-checkout and exact-revision Linux CI are merge gates;
no claim of feature freeze is made until both branch and merged main CI succeed.
