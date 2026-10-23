# Owned rollback integration gate (production inactive)

This gate exercises the [internal owned cleanup protocol](owned-rollback-protocol.md),
not a production feature switch. Ordinary BEGIN and startup recovery remain legacy
by default. The production exclusive-writer rule, WAL/page/checkpoint formats,
and protected experimental branches are unchanged.

## Test admission

`OwnedSqlTestAccess` is defined only in the integration test. Its two private
friend declarations provide access to the real TransactionManager session lease
and SqlExecutor, not a public activation API. It acquires the ordinary session
latch and mutation admission, binds the existing lazy transaction through the
Stage-2 harness, invokes the real executor, then uses ordinary completion/error
rollback. Parsing, reads and BEGIN/COMMIT/ROLLBACK use SqlEngine directly. This
small mutation dispatcher mirrors SqlEngine's existing dispatch; it must not be
mistaken for testing a production-owned autocommit configuration.

The TCP cases are different: requests run through the unchanged TcpServer and
SqlEngine. After an acknowledged, still-lazy BEGIN, a pipe-controlled test binds
the real exclusive session lease before allowing requests to continue. No TCP
feature option, grammar change, callback or environment activation exists.

## Coverage and assertions

- SQL INSERT, UPDATE, DELETE, multi-statement rollback/commit, duplicate-key
  errors, implicit post-extension failure, CREATE rollback/commit and reopen.
- Real Catalog initialization/root rollback; Table, slotted heap relocation and
  persistent index split/merge/root restoration. The empty index's existing
  logical capacity fields are set to 3/3 through ordinary WAL-protected guards
  in test setup, so 80 rows exercise internal splits without enormous datasets.
- Exact rows and RIDs, catalog root, original index root, retained extent,
  complete allocator membership and heap/index/catalog/buffer validators.
- 180 SQL cleanup hard-crash/control cases: ten cleanup boundaries, three WAL
  modes, two stores, and intact/missing/corrupt-newest checkpoint control.
  Each reopens twice, verifies exact free membership, checkpoints completed
  cleanup, reuses pages for a new table and reopens again.
- Actual TCP disconnect and SIGKILL of a server process with an uncommitted
  transaction; original committed rows/RIDs and catalog must survive restart.
- All Stage-2 directed, negative, authority, two-write-order and synthetic
  P101-loser/P102-winner assertions remain unchanged.

The model adapts the preserved prototype's ordered release/update/allocation
workload, oracle and four seeds (0x12B3A4, 0xC0FFEE, 0xDE1A, 0xA110C), using the
current private authority and a genuine checkpoint lease. Each seed requires
at least 7,500 **completed lifecycle events**. Planned or failed actions and
comparison assertions are not credited. Opaque committed page bytes, exact LIFO
free membership, PageLSN-bearing Free Pages, complete extent accounting and
catalog-root isolation are checked after every transaction and reopen. Real
STEAL, reservations, CLRs, rollback, fuzzy/sharp checkpoints, segment deletion,
hard exits and repeated interrupted intent-tail recovery are included. Failure
reports identify seed, cycle, completed count, phase, commit/crash and reopen
state; the first failure stops the gate. This is single-writer testing, not
overlapping allocator admission.

## Commands and measurement

Reuse the warning-enabled Release cache. Build new targets explicitly; the
workflow helper's static focused selectors do not include these new tests.

```sh
cmake -S . -B build-retirement-final
cmake --build build-retirement-final --parallel 6 --target \
  owned_rollback_sql_test owned_rollback_model_test
ctest --test-dir build-retirement-final --parallel 1 --output-on-failure \
  --no-tests=error -R '^owned_rollback_sql(_crash|_tcp)?_test$'
python3 scripts/dev-verify.py --profile FOCUSED --area allocator-recovery \
  --build-dir build-retirement-final --jobs 6
python3 scripts/dev-verify.py --profile FOCUSED --area sql-parser \
  --build-dir build-retirement-final --jobs 6
# Final candidate: includes the complete four-seed model, once, plus every old target.
python3 scripts/dev-verify.py --profile FULL --area all \
  --build-dir build-retirement-final --jobs 6
```

The previous Stage-2 timing discrepancy was within the same recorded Release
invocation, not evidence of two configurations: `.dev-verify.jsonl` records
28.1265s build, 0.0295s inventory and 240.8056s CTest using Python `perf_counter`
(268.98s helper total). CTest reported 1062.09s; its retained LastTest.log spans
13:18–13:36 on October 23. The log identifies the same 97-test final candidate,
based on a dirty a019b0e subsequently committed as c66aec2/f86cb4a. No sleep or
clock-adjustment trace establishes the cause of the clock disagreement. Do not
combine or normalize those observations. New focused/model commands report
wall-clock elapsed separately from helper monotonic and CTest-reported values;
record command, source candidate, build/CTest time, count and exit status.

### Recorded Release candidate

Based on HEAD `f86cb4a`, staged candidate tree
`ccf3369d587e456635549e9c60a61ca24374f942`; subsequent result documentation does
not change the tested executable sources, committed as `85f9379`. AppleClang 21.0.0, Release,
`-Wall -Wextra -Wpedantic`; no compiler warnings observed. All commands above
use serial CTest; compilation uses six jobs. Recorded seconds:

| Profile / selection | Build | CTest reported | Helper total | Result |
| --- | ---: | ---: | ---: | --- |
| FAST allocator-recovery | 1.456 | 0.87 | 2.397 | 7/7 |
| FOCUSED allocator-recovery | 12.120 | 46.90 | 59.095 | 22/22 |
| FOCUSED sql-parser | 4.959 | 11.66 | 16.679 | 11/11 |
| FULL all registered targets | 8.748 | 245.16 | 253.957 | 101/101 |

For this FULL invocation an external `time.time()` wrapper measured 254.003s,
its `perf_counter()` measured 254.001s, and the helper measured its CTest command
at 245.164s. Those observations are consistent. All 97 previous targets remain;
four added registrations are model, SQL directed, SQL crash and SQL TCP. No
failure was disabled. An initial TCP harness called SELECT after closing the
server gate; that harness error was corrected by verifying before shutdown,
without changing database code or any expected result.

Final FULL model results (comparison assertions excluded from event counts):

| Seed | Completed lifecycle events | Transactions | Initial hard exits | Additional recovery interruptions | Reopens | Checkpoints | Segments deleted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0x12B3A4 | 7,518 | 479 | 69 | 12 | 106 | 81 | 1,715 |
| 0xC0FFEE | 7,503 | 462 | 66 | 12 | 102 | 78 | 1,824 |
| 0xDE1A | 7,507 | 469 | 67 | 18 | 104 | 80 | 1,763 |
| 0xA110C | 7,521 | 464 | 67 | 10 | 103 | 79 | 5,935 |

Total: 30,049 completed events, 1,874 completed transactions, 269 initial hard
exits, 52 further recovery interruptions, 415 reopens, 318 checkpoints and 11,237
observed segment deletions. Each seed separately asserts actual deletion, exact
physical extent accounting and exact current free-list membership.

### Sanitizer gates

Use the separate ASan/UBSan and TSan configurations documented in
[the workflow](development-workflow.md#release); do not combine them. The exact
additional targets/selections used on the same executable source candidate:

```sh
cmake --build build-retirement-asan --parallel 6 --target \
  owned_rollback_codec_test owned_rollback_cleanup_test owned_rollback_model_test \
  owned_rollback_sql_test reclaim_lineage_test physical_writeback_test \
  frame_wal_retirement_test runtime_clr_rollback_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-retirement-asan --parallel 1 --output-on-failure --no-tests=error \
  -R '^(owned_rollback_(codec|cleanup|cleanup_crash|model|sql|sql_crash|sql_tcp)_test|reclaim_lineage_test|physical_writeback_test|frame_wal_retirement_test|runtime_clr_rollback_test)$'

cmake --build build-retirement-tsan --parallel 6 --target \
  buffer_concurrency_test concurrent_sessions_test concurrent_tcp_test \
  database_access_gate_test frame_wal_retirement_test
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-retirement-tsan --parallel 1 --output-on-failure --no-tests=error \
  -R '^(buffer_concurrency_test|concurrent_sessions_test|concurrent_tcp_test|database_access_gate_test|frame_wal_retirement_concurrency_test)$'
```

Both caches were explicitly reconfigured with `cmake -S . -B <build-dir>` before
building, retaining their distinct warning-enabled sanitizer flags. ASan/UBSan:
14.066s configure/build, 233.08s CTest (233.105s external wall), **11/11 passed**,
no findings; includes the complete four-seed model and SQL crash matrix. Leak
detection was disabled, so this is not LSan coverage. TSan: 16.253s configure/build,
10.97s CTest (10.997s external wall), **5/5 passed**, no findings. Linux/GCC CI and
fresh-clone release checks are deferred to the separately approved activation gate.

## Legacy audit and limits

The Stage-2 delta repair changes only the physical base used by appended-page
delta REDO (`beforePageExisted=false`). Such deltas are computed against the
transaction's original zero before-image, not the previous logged delta or a
later typed/free incarnation. Existing-page deltas keep their prior base. Codec,
record lengths and PageLSN validation are unchanged. A minimal ByteRange append
two pages / commit / release second page / commit / reopen reproduction exists
against the unchanged a019b0e library; `legacyAppendDeltaBase()` in the unchanged
Stage-2 directed test covers all modes, stores and both REDO policies.

There is no general allocation generation, reachability framework or multi-writer
protocol. Raw concurrent administrator writes and byte-identical unlogged ABA
are outside the exclusive lifecycle contract; identical bytes cannot establish
or reject a distinct physical identity without additional evidence. Known
conflicts and changed/unaccounted physical images must fail closed. The synthetic
committed higher page test is not permission to overlap production allocation.

Even a green integration gate is not activation. Final review must establish
production startup/session authority lifetime, replace only test authorization
with a private production authority path, deliberately enable owned BEGIN and
owned recovery together, and repeat applicable compatibility/crash/sanitizer
gates. Legacy BEGIN must retain truncation. No such activation is implemented here.

Minimal subsequent activation plan, requiring separate approval:

1. Review and establish a real exclusive startup lifecycle authority before pool
   or SQL admission; keep the gate/lease lifetimes valid through recovery and
   session rollback. Do not simply remove the inactive guard or trust analysis
   snapshots. No public constructor/SQL/network/environment feature switch.
2. Bind ordinary production writer contexts to that reviewed authority and
   enable owned BEGIN emission and authorized owned startup recovery together.
   Retain legacy dispatch, all ownership/conflict checks and the exclusive writer.
3. Rerun the complete model, SQL/crash/retention and legacy gates, ASan/UBSan,
   TSan, Linux/GCC CI and fresh-clone compatibility before a separately reviewed
   merge. Do not enable general multi-writer allocation or raw administrative ABA.
