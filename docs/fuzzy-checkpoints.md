# Fuzzy checkpoints, the Dirty Page Table, and recLSN

MiniDB++ supports two checkpoint modes. `sharp` remains the default: it writes every
dirty buffer frame and synchronizes `database.db` before publishing the checkpoint.
`fuzzy` is opt-in: it publishes recovery metadata while dirty page bytes remain in the
buffer pool. Both modes use the existing double-slot `.ckpt` control format.

This is a deliberately serial subset of ideas used by ARIES-style recovery. The
[original ARIES paper record](https://research.ibm.com/publications/aries-a-transaction-recovery-method-supporting-fine-granularity-locking-and-partial-rollbacks-using-write-ahead-logging)
describes a broader protocol with transaction and dirty-page tables, PageLSNs, fuzzy
checkpoints, CLRs, locking, and partial rollback. MiniDB++ is not ARIES-compliant: it has
one implicit statement transaction at a time, no CLRs, and currently publishes fuzzy
checkpoints only between statements.

## Dirty Page Table ownership and lifetime

Each resident buffer frame owns its `recLSN`: the exact WAL record LSN of the earliest
update in its current dirty period. The first logged update of a clean frame assigns both
PageLSN and recLSN. Later updates advance PageLSN but retain recLSN. A successful page
write or dirty eviction clears recLSN only after the write succeeds; a later update
starts a new dirty period with a new recLSN. `BufferPoolManager::dirtyPageTableSnapshot()`
returns a canonical PageId-ordered metadata snapshot without reading mutable page bytes.

Because the current mutation API discovers the physical update at statement commit,
write intent can temporarily mark a frame pending-dirty before an LSN exists. A fuzzy
checkpoint cannot run in that interval: the no-active-statement restriction guarantees
that every snapshotted dirty frame has its exact recLSN and PageLSN assigned.

## WAL record types and byte layouts

Both records are system records (`transactionId = 0`, `prevLSN = INVALID_LSN`). All
integers are little-endian and the ordinary WAL envelope supplies length validation and
CRC32C.

`FUZZY_CHECKPOINT_BEGIN` is WAL type 11 with a fixed 32-byte payload:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | format version (`1`) |
| 2 | 2 | header/payload size (`32`) |
| 4 | 4 | flags (`0`) |
| 8 | 8 | checkpoint ID |
| 16 | 8 | previous authoritative checkpoint END LSN or `INVALID_LSN` |
| 24 | 8 | reserved (`0`) |

`FUZZY_CHECKPOINT_END` is WAL type 12. Its fixed header is 64 bytes:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | format version (`1`) |
| 2 | 2 | header size (`64`) |
| 4 | 2 | DPT entry size (`16`) |
| 6 | 2 | ATT entry size (`48`) |
| 8 | 8 | checkpoint ID |
| 16 | 8 | matching fuzzy BEGIN LSN |
| 24 | 8 | database page count |
| 32 | 8 | next transaction ID |
| 40 | 4 | DPT entry count |
| 44 | 4 | ATT entry count |
| 48 | 16 | reserved (`0`) |
| 64 | variable | PageId-ordered DPT entries, then transaction-ID-ordered ATT entries |

Each DPT entry is 16 bytes:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | nonzero, non-`INVALID_PAGE_ID` PageId |
| 4 | 4 | reserved (`0`) |
| 8 | 8 | valid recLSN |

Each future-compatible ATT entry is 48 bytes:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 8 | nonzero transaction ID |
| 8 | 2 | status (`1` = `ACTIVE`) |
| 10 | 2 | reserved (`0`) |
| 12 | 4 | reserved (`0`) |
| 16 | 8 | BEGIN LSN |
| 24 | 8 | last LSN, at or after BEGIN |
| 32 | 8 | statement start database-page count |
| 40 | 8 | reserved (`0`) |

The codec rejects unsupported versions and sizes, unknown status values, noncanonical or
duplicate IDs, invalid LSN relationships, nonzero reserved bytes, arithmetic/record-size
overflow, and trailing bytes. Production checkpoints currently encode an empty ATT and
recovery rejects a nonempty checkpoint ATT: transaction-overlap checkpointing is
explicitly deferred.

## Publication, restart, and retention

Fuzzy publication appends BEGIN, snapshots the DPT/empty ATT, appends END, fsyncs WAL
through END, then writes and fsyncs the inactive control slot. Segment rotation and
reclamation follow publication. It does not call `flushAll()`, write database pages, or
fsync `database.db`. Pinned frames are allowed because only frame recovery metadata is
copied. Failures before control durability leave the prior generation authoritative;
reclamation failures can only retain extra history.

For fuzzy recovery, analysis starts at BEGIN. END seeds the restart DPT. Subsequent
updates add missing pages and conservatively retain existing recLSNs because page flushes
are not WAL-logged. Physical REDO starts at the minimum restart recLSN. An update at `R`
is rejected without a page read when its page is absent or `R < recLSN`; a surviving
PageLSN-aware update is skipped when persistent `PageLSN >= R`. Legacy updates use the
safe PageId/recLSN filter but never PageLSN skipping. Existing winner/loser analysis and
serial loser UNDO remain unchanged.

The retention floor is `min(fuzzy BEGIN, minimum DPT recLSN, minimum ATT BEGIN if
supported)`, plus one retained predecessor segment. A page with an old recLSN therefore
pins history. After it is flushed, a later fuzzy snapshot omits it or records a new dirty
period, allowing the floor to advance. Sharp checkpoints remain useful for an empty DPT,
simpler recovery, and aggressive reclamation.

Neither mode makes multi-file operations crash-atomic. There is no background writer,
CLR, concurrent transaction, locking, MVCC, WAL archive, or torn-page recovery.
