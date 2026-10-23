# Owned rollback WAL analysis (inactive)

The owned rollback extension provides codecs, a side-effect-free history validator,
and an **internal test-authorized cleanup executor**. Production emits ordinary BEGIN, uses CLR UNDO and
global truncation, and retains its exclusive writer gate. There is no public
protocol switch. Default recovery rejects owned records in its recovery window before
tail repair, page REDO, CLR generation or physical cleanup. Only a private friend
harness defined in the tests can bind an exclusive lifecycle lease and exercise
owned BEGIN generation/recovery. This is not production activation.

## Prototype-compatible bytes

The existing 48-byte WAL header, checksum, types 1–14, database v2 and all
checkpoint/page layouts are unchanged. These additional user-owned records have
nonzero TransactionId; all integers are little-endian. Common payload prefix:
offset 0:u16 version=1, 2:u16 exact payload size, 4:u32 flags=0.

| ID / record | Payload bytes | Remaining offsets:width and meaning |
| --- | ---: | --- |
| 15 BEGIN_OWNED | 24 | 8:8 original startPageCount; 16:8 reserved=0 |
| 16 ROLLBACK_CLEANUP_BEGIN | 24 | 8:8 owned BEGIN LSN (logical UNDO endpoint); 16:8 reservation count |
| 17 ROLLBACK_CLEANUP_INTENT | 72 | 8:8 reserveLSN; 16:8 associated system reclaimLSN; 24:48 unchanged PAGE_RECLAIM payload |
| 18 ROLLBACK_CLEANUP_PROGRESS | 40 | 8:8 reserveLSN; 16:8 reclaimLSN or INVALID_LSN for absence; 24:4 PageId; 28:4 reserved=0; 32:8 observed extent |

Complete record widths are 72, 72, 120 and 88 bytes. The nested reclaim fields
are page 24:4, prior free head 28:4, version 32:2, flags 34:2, reserved 36:4,
extent 40:8, before-pageLSN 48:8, before-metadataLSN 56:8, CRC32C 64:4,
reserved 68:4. See [the type-14 contract](crash-safe-page-reclamation.md).

The BEGIN prevLSN is INVALID_LSN. Other new records follow the owner's exact
lastLSN. A system PAGE_RECLAIM has TransactionId=0 and never becomes that user's
prevLSN. The intent explicitly names its immediately following record boundary;
`cleanupReclaimLsn()` is the single checked calculation. It does **not** append,
reserve a WAL position, or complete a missing record. An intent-only scanned tail
remains unfinished evidence to analysis. The authorized executor can complete that
exact WAL tail only after validating owner, UNDO endpoint, materialization and
unchanged target/page-0 preconditions. It forces the associated type-14 record
before physical REDO. An occupied/different/non-tail association is corruption.

Decoders check identity, exact length/version/flags/reserved fields, legal page
counts/IDs, backward references, overflow and nested reclaim encoding. Encoders
reject invalid payload values instead of silently canonicalizing them. The real
WAL codecs/scanners still handle outer record boundaries, checksums, unsupported
future IDs, malformed lengths and torn tails. Older binaries cannot read IDs 15–18.

## Analysis and ownership evidence

`analyzeOwnedRollbackHistory()` accepts an existing scanner's complete, contiguous,
BEGIN-rooted record sequence. It is not a replacement for normal checkpoint-aware
recovery, a scanner, or an arbitrary retained-suffix validator. It returns owned
value snapshots and has no DiskManager, LogManager writer, gate or reclaimer.

Each transaction has explicit LegacyTruncate/OwnedAppend discrimination. Each
APPENDED reservation binds TransactionId, PageId, reserveLSN and expected old
extent through the validated user chain. Page updates must have matching existence
flags; an owned appended update needs its reservation. Preexisting updates,
including free-list reuse/release, remain separate before-image/CLR work. No new
reuse-source reservation or claim of complete reuse provenance is introduced.

`materializedInRuntime=true` exists only in the existing live append-confirmation
context. Analysis always reconstructs false, meaning **unknown after restart**,
not absent. A recorded absence outcome is also distinct from runtime confirmation.
Neither matching PageId, CRC, extent nor absence of pins supplies cleanup authority.

The validator checks exact transaction-local chain/boundary ownership, conflicts,
duplicate BEGIN/reservations, matching CLR owner/page/original update/undoNext and
the next required UNDO operation. It rejects skipping or repeating existing-page
UNDO, mixing legacy cleanup, mutation/COMMIT after cleanup and post-terminal records.

## Cleanup state machine: claims versus authority

```text
OwnedActive -> [Undoing through ordinary CLRs]
            -> LogicalUndoComplete (verified chain endpoint)
            -> CleanupStarted
            -> ReservationCleanupInProgress -> CleanupComplete
            -> AbortRecorded
```

The CLEANUP_BEGIN marker proves the **WAL-chain** endpoint only. It does not prove
that compensated page bytes have been synchronized. Each intent must identify a
unique owned reservation and exactly matching subsequent type-14 record. Reclaim
progress requires that association and matching extent; repeated completion fails.
Empty reservation sets complete at the marker.

There is one deliberate prototype exception: absent-page progress has INVALID
reclaimLSN and no intent, with PageId >= observed extent >= original extent. This
records an absence claim, not a physical reclaim. It must not be confused with
progress missing its required intent for a materialized page.

`AbortRecorded` means a complete terminal WAL record was supplied. The analyzer
cannot observe WAL fsync and does not label it durable. The executor must
establish physical absence, exclusive lifecycle authority, logical image restoration,
page-0 synchronization and free-list completion **before** recording/forcing ABORT.
The validator neither issues certificates nor authorizes PAGE_RECLAIM alongside a
live transaction. It only checks that an explicitly associated proposed history is
structurally valid; production owned physical cleanup remains disabled.

## Internal physical execution

Both live and startup cleanup require the same private exclusive gate/lease
binding. No public constructor flag, SQL/CLI option or environment activation
exists. Existing `PageReclaimer::reclaim()` and its idle/certificate checks are
unchanged; the executor reuses the same type-14 codec, canonical Free Page builder,
two-page REDO, preconditions and transitive later-state validation.

The bounded contract excludes raw administrative append/write/reuse during a live
transaction. A target needs a validated owner BEGIN/reservation/chain, completed
user UNDO, exclusive authority, physical presence, no conflicting owner, no catalog
reference and no existing free-list membership. Its bytes must match the canonical
zero append result or an exact reconstructed same-owner WAL after-image. This does
not rely on a volatile confirmation, CRC alone, pins, transaction count or an ID
suffix. A physical suffix coverage check additionally rejects unreserved growth;
independently committed reservations can account for unrelated higher pages but
never authorize reclaiming them. Opaque/unlogged changed bytes fail closed.

```text
validate exclusive lease and pins -> prepare/force latest user WAL
  -> invalidate only touched/reserved frames and explicitly rebuilt page 0
  -> ordinary CLR UNDO of existing pages -> database fsync
  -> append/force CLEANUP_BEGIN (original BEGIN LSN + reservation count)
  -> each APPENDED reservation:
       absent: record absence, never extend the file
       present: prove bytes/ownership/nonmembership
          -> append/force INTENT -> append/force associated PAGE_RECLAIM
          -> write Free Page and page-0 head -> database fsync
       append/force PROGRESS
  -> database fsync -> append/force ABORT
```

Preexisting free-list reuse/release remains ordinary before-image/CLR work. User
page-0 compensation completes before new head publication. An appended page released
inside its transaction is unlinked by that compensation before orphan cleanup;
surviving duplicate membership is rejected. A completed progress record is verified
against canonical free membership or physical absence when an unfinished cleanup
resumes. Already-recorded reclaims are replayed, never blindly appended again.
Completed historical absence may coexist with a page only if a later independently
validated append reservation accounts for that new physical identity.

Live invalidation does not discard a historical page-count suffix. Unrelated resident
bytes, dirty state and PageLSN remain intact. Pins are conservatively rejected through
the existing completion rule, and unrelated dirty page-0 state cannot be discarded.
Startup has no resident frames. A failure poisons the live rollback until restart;
new transactions/checkpoints cannot advance over incomplete cleanup. Fuzzy ATT
retains BEGIN/lastLSN at safe statement boundaries. Authorized startup also audits
retained owned history against the checkpoint ATT, rejecting omitted live owners.
This extra retained-WAL audit is not enabled on ordinary production recovery.

Legacy truncating UNDO and winner guards remain. Directed testing exposed a separate
legacy REDO defect: an old appended-page byte-range update could patch a later Free
Page while retaining its newer PageLSN, causing selective REDO to skip required
restoration. Appended deltas now reconstruct from their defined zero-page base (both
delta encodings), not a later physical incarnation. Existing-page deltas and all WAL
bytes remain unchanged. The minimal legacy reproduction also fails against the
unchanged pre-executor library; the directed test covers all modes/stores/REDO policies.

The directed executor checks 600 bounded rollback cycles, absent/uninitialized
appends, mixed reuse/release, metadata restoration, higher committed resident pages,
repeated startup interruptions, checkpoint blocking and malformed authority/history.
The crash target has 504 hard-exit prefixes across three update modes, two WAL stores,
both REDO policies and both reclaim write orders, with two restarts per prefix.
Another 18 cases interrupt the recovery-only intent-tail append/force itself,
each with two hard exits and two successful restarts, checking one reclaim record.
These are the executor-level tests, separate from the integration gate below.

The separate [integration gate](owned-rollback-integration.md) exercises the full
owned model, table/index/catalog, disconnect and retention/reuse paths. It records
test admission and verification results; production activation still requires review.
Shared allocation, multiple writers, epochs, deferred release publication, new
page/checkpoint formats, SQL and wire changes are not included. Raw byte-identical
administrative ABA cannot be detected without generations; it is excluded by the
exclusive lifecycle contract, not claimed as supported concurrency.
