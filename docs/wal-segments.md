# Segmented WAL lifecycle

Milestone 11C.2 stores the active write-ahead log as one monotonically advancing
logical byte stream split across files. PostgreSQL's
[WAL configuration](https://www.postgresql.org/docs/current/wal-configuration.html)
and [continuous-archiving](https://www.postgresql.org/docs/current/continuous-archiving.html)
documentation provide architectural background for segment lifecycle and retention,
but MiniDB++ uses its own small format, sharp checkpoints, full-page records, and
synchronous single-threaded implementation.

For `database.db`, active WAL lives in:

```text
database.db.wal.d/
    manifest
    0000000000000001.seg
    0000000000000002.seg
    ...
```

Names are fixed-width lowercase hexadecimal `WalSegmentId` values. Segment ID zero is
invalid. IDs increase and are never reused; they are independent of transaction,
checkpoint, and LSN identities. Headers, not filenames or directory enumeration order,
are authoritative.

## Global logical LSN

`Lsn` remains `uint64_t`, but means a byte position in the abstract WAL stream rather
than an offset in one file. The first position remains 64 for compatibility. Record
bytes are logically contiguous: unused capacity at the end of a physical segment does
not create an LSN gap. Reclamation never rewrites a record, LSN, or `prevLSN`; a later
record keeps the same identity after every earlier segment has disappeared.

For a segment starting at `S`, logical LSN `X` maps to:

```text
local file offset = 64 + (X - S), for S <= X < segment logical end
```

Only segmented storage performs this mapping. The existing 48-byte record layout and
CRC are unchanged: its 64-bit LSN bytes now carry the global meaning, so legacy records
remain byte-compatible.

## Segment header (version 1, 64 bytes)

All integers are unsigned little-endian. CRC32C covers all 64 bytes with bytes 56–59
treated as zero.

| Offset | Width | Field | Version-1 invariant |
| ---: | ---: | --- | --- |
| 0 | 8 | magic | ASCII `MDBWSEG1` |
| 8 | 4 | format version | `1` |
| 12 | 4 | header size | `64` |
| 16 | 8 | segment ID | nonzero, increasing |
| 24 | 8 | start LSN | first logical byte mapped by payload |
| 32 | 8 | previous segment ID | zero only for segment 1 |
| 40 | 8 | previous logical end LSN | equals this start LSN |
| 48 | 4 | maximum payload capacity | production default `16,777,216` |
| 52 | 4 | flags | `0` |
| 56 | 4 | header CRC32C | Castagnoli |
| 60 | 4 | reserved | `0` |

One record must fit wholly within one segment. If it cannot fit in the active
segment's remaining payload, rotation fsyncs the predecessor, creates and writes the
successor header, fsyncs the successor file, then fsyncs the WAL directory before
append continues. The predecessor is thereafter immutable. Only the highest segment
may contain a repairable truncated final record; a short record in a closed segment is
corruption.

Open sorts segment IDs and validates filenames, CRCs, persisted capacity, increasing
IDs, predecessor references, contiguous logical ranges, record boundaries/CRCs, and
that no required interior segment is absent. An unexpected segment-shaped filename is
rejected. Unrelated non-segment files are ignored.

## Manifest (version 1, 64 bytes)

The manifest proves that the directory is a published MiniDB++ segmented store. It is
small and not rewritten during ordinary appends.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | ASCII `MDBWSM01` |
| 8 | 4 | version `1` |
| 12 | 4 | header size `64` |
| 16 | 4 | persisted segment payload capacity |
| 20 | 4 | flags, zero |
| 24 | 8 | first retained segment ID |
| 32 | 8 | initial logical LSN (`64`) |
| 40 | 8 | first migrated legacy LSN, or `INVALID_LSN` for a native store |
| 48 | 8 | reserved zero |
| 56 | 4 | CRC32C with this field zeroed |
| 60 | 4 | reserved zero |

The production capacity is 16 MiB. Tests/benchmarks may create a smaller persisted
capacity; reopen must supply the same value. This is not a per-startup format toggle.

## Sharp checkpoint and reclamation

After database sync, durable `CHECKPOINT_END`, and durable control publication, the
checkpoint critical path is complete. MiniDB++ then synchronously rotates to a fresh
active segment and separately measures best-effort reclamation. It conservatively
retains:

- the complete segment(s) needed for the newest sharp checkpoint BEGIN/END pair;
- every later segment, including the active segment;
- one additional closed predecessor as a local safety/debug cushion.

A closed segment is deleted only when its complete logical interval precedes that
floor. Reclamation first atomically publishes the new `firstRetainedSegmentId`, then
unlinks eligible whole files and fsyncs the WAL directory. A crash may leave extra old
files, which are ignored below the published floor; required files are never deleted
before the new floor is durable. LSNs and remaining files are never rebased or compacted.

The sharp checkpoint is quiescent, so no active transaction's `prevLSN` chain crosses
behind the floor. If that invariant could not be established, reclamation would be
unsafe and must not proceed.

If `database.db.ckpt` is missing or corrupt after reclamation, recovery scans retained
segments, finds the newest matching durable checkpoint pair, starts from its logical
recovery LSN, and best-effort rebuilds the control file. `CHECKPOINT_END` is safe
without control publication because database fsync preceded END fsync. Thus the
control sidecar is not a single point of recoverability.

## Legacy migration

`database.db.wal` remains the legacy v1 monolithic path. Production startup prefers a
valid published segmented directory. Otherwise it opens legacy WAL, completes normal
REDO/UNDO (including durable ABORT for a loser), takes a sharp checkpoint, and migrates
the complete compatible logical record stream.

Migration builds `database.db.wal.d.tmp`, writes/fsyncs its manifest and segment files,
fsyncs the temporary directory, atomically renames it to `database.db.wal.d`, and
fsyncs the parent directory. The legacy file is kept until publication is durable,
then unlinked followed by another parent-directory sync. A crash before rename leaves
legacy authoritative; a crash after rename makes the valid segmented directory
authoritative even if legacy still exists. Abandoned temporary directories are never
selected and are removed on startup.

## Metrics, bounds, and limitations

Diagnostics distinguish monotonically generated logical bytes/LSN high water from
currently retained physical bytes, and expose rotations, created/closed/deleted
segments, directory syncs, active ID, oldest retained LSN, and reclaimed bytes.
`wal_segment_rotation` and `wal_reclamation` use deliberately small test segments and
must not be quoted as 16-MiB production performance.

With periodic sharp checkpoints, physical WAL is broadly bounded by the retained
checkpoint segment(s), one extra predecessor, post-checkpoint tail, and active segment.
A single large statement can exceed the checkpoint target because checkpoints never
run inside an active transaction. There is no archive/PITR mode: reclaimed files are
gone. Replication or archival would require another retention constraint.

The protocol guarantees clean flush/reopen persistence and tested process-crash
boundaries, not arbitrary power-loss atomicity across database, WAL, control, and
directory operations. There is still no persistent pageLSN, fuzzy checkpoint, CLR,
physiological/byte-range logging, concurrent transaction, locking, MVCC, background
writer, group commit, or WAL archive.
