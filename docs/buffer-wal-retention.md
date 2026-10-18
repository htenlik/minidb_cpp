# Buffer PageLSNs and retained WAL

Three LSN concepts have different lifetimes:

- **Persistent PageLSN** is historical metadata in the page bytes. A safe checkpoint
  can make its originating WAL unnecessary without changing those bytes.
- **Volatile frame pageLSN** tracks the buffered WAL-before-data dependency.
- **recLSN** is the first update of the current dirty period; later updates preserve
  it, and successful writeback clears it. Writeback is not database fsync.

[Physical writeback tracking](physical-writeback-durability.md) now distinguishes
already-written unsynchronized state from synchronized database state. Fuzzy
checkpoints conditionally fsync those physical writes before publication, while
still-dirty frames retain their ordinary DPT entries and WAL dependencies.

## Remaining clean resident frame defect

BufferPoolManager currently leaves a clean frame's old volatile pageLSN in place
after writeback. `validate()` requires it to remain in the WAL provider's retained
record set. Legitimate checkpoint/reclamation can remove that historical record
and cause `Buffer frame pageLSN is unknown to its WAL provider`.

This separate bookkeeping defect is still reproducible with:

```sh
./build/reclaim_lineage_test --frame-diagnostic
```

It is deliberately not repaired or registered as an expected pass here. The
preserved audit branch and its red tests are unchanged. Do not skip all WAL
validation, redefine `containsLsn`, erase persistent PageLSN bytes or interpret
an arbitrary unknown dirty dependency as retired history.

A future explicit retirement lifecycle must use a safe synchronization/checkpoint
authorization, retain strict dirty/recLSN checks, preserve frame identity/pinning,
and allow subsequent writes to establish fresh dependencies. The writeback fence
does not itself clear any frame LSN or change buffer guard semantics.
