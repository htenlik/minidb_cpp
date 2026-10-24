# Development workflow

Use Python 3.8+ (standard library only), CMake 3.20+, and the existing C++20
toolchain. Run from the repository root. Do not install dependencies to use the
helper. Keep one build directory per compiler/configuration and reuse it.

## Helper contract

`python3 scripts/dev-verify.py` requires `--profile`, `--area`, and `--build-dir`.
Areas are `buffer-wal`, `allocator-recovery`, `sql-parser`, `network`, and `all`.
FAST/FOCUSED require a specific area; FULL/RELEASE require `all`. Unknown values
are errors. Relative build paths resolve from the repository root.

- Missing caches are configured once as warning-enabled Release builds.
- Existing single-configuration Makefiles/Ninja caches are reused, not deleted or
  silently reconfigured. A cache belonging to another source tree is rejected.
- FULL requires Release; FAST/FOCUSED can reuse a matching instrumented cache.
- `--jobs N` changes compilation parallelism only (default 2). CTest always gets
  `--parallel 1`, even if `CTEST_PARALLEL_LEVEL` is set in the environment.
- FAST/FOCUSED build only selected executable targets, resolving CTest aliases.
  The helper checks CTest registration and rejects missing or empty selections.
- FULL builds all targets and runs CTest without include/exclude filters. Future
  registered tests are included automatically; there is no fixed test-count ceiling.
- No tests are skipped because a Git SHA was previously verified. There are no
  retries, automatic benchmark repetitions, test disabling, or seed reductions.
- `--dry-run` prints commands without executing tools or writing files/logs.
  `--help` is also nonmutating.

Existing local directories include `build-retirement-final`,
`build-retirement-asan`, `build-retirement-tsan`, and `build-retirement-gcc`.
The examples reuse the Release cache; a fresh clone can use the same name or
another explicit path. Reconfigure deliberately when CMake/toolchain/flags change
or release benchmark Git metadata needs refreshing; ordinary edits need only an
incremental build. Do not overlap builds/tests using the same directory, or run
separate suites concurrently against fixed temporary filenames.

## FAST

Run after a small edit. Choose only relevant areas; FAST is not a merge gate for
cross-layer recovery changes. These are four alternative commands, not a mandate
to execute every area after every edit:

```sh
python3 scripts/dev-verify.py --profile FAST --area buffer-wal --build-dir build-retirement-final
python3 scripts/dev-verify.py --profile FAST --area allocator-recovery --build-dir build-retirement-final
python3 scripts/dev-verify.py --profile FAST --area sql-parser --build-dir build-retirement-final
python3 scripts/dev-verify.py --profile FAST --area network --build-dir build-retirement-final
```

| Area | FAST membership |
| --- | --- |
| buffer-wal | `buffer_pool_manager_test`, `buffer_pool_wal_test`, `dirty_page_table_test` |
| allocator-recovery | `page_allocator_test`, `wal_codec_test`, `recovery_log_test`, `clr_codec_test`, `checkpoint_codec_test`, `fuzzy_checkpoint_codec_test`, `page_lsn_test`, `owned_rollback_codec_test` |
| sql-parser | `sql_lexer_test`, `sql_parser_test`, `sql_parser_robustness_test`, `sql_semantics_test` |
| network | `wire_protocol_test`, `tcp_transport_test` |

## FOCUSED

Run after a coherent change, before its commit. Includes the area's FAST tests
plus affected integration/recovery/directed crash tests. These are explicit lists,
not automatic dependency analysis: select additional areas for cross-layer edits.

```sh
python3 scripts/dev-verify.py --profile FOCUSED --area buffer-wal --build-dir build-retirement-final
python3 scripts/dev-verify.py --profile FOCUSED --area allocator-recovery --build-dir build-retirement-final
python3 scripts/dev-verify.py --profile FOCUSED --area sql-parser --build-dir build-retirement-final
python3 scripts/dev-verify.py --profile FOCUSED --area network --build-dir build-retirement-final
```

| Area | Total tests | Integration emphasis |
| --- | ---: | --- |
| buffer-wal | 14 | Frame retirement, physical writeback, reclaim lineage, CLR/UNDO, fuzzy checkpoints, concurrent buffer/readers, SQL storage integration |
| allocator-recovery | 25 | Reservation/append crashes, owned cleanup and directed cleanup crashes, reclamation, pageLSN/segmented recovery, checkpoints, transaction rollback, legacy migration |
| sql-parser | 13 | Executor/random execution, schema tuple codec, Table/Catalog, owned SQL integration and default production rollback |
| network | 10 | Protocol/stress, concurrent sessions/TCP, explicit transactions, owned TCP cleanup and default production startup/cleanup crashes |

The `FAST`/`INTEGRATION` lists in [the helper](../scripts/dev-verify.py) are
authoritative. Inspect the exact targets and anchored test-name regex with:

```sh
python3 scripts/dev-verify.py --profile FOCUSED --area buffer-wal --build-dir build-retirement-final --dry-run
```

**`page_reclaim_test` is intentionally not FAST.** Its default executable contains
the complete 20,000-event model. Allocator FOCUSED includes this expensive test;
the helper neither splits nor trims it. Other directed binaries also include
hard-crash matrices; FOCUSED is not a promise of subsecond execution.

Owned coverage follows the boundary being changed: allocator recovery selects
`owned_rollback_cleanup_test` and `owned_rollback_cleanup_crash_test`; SQL selects
`owned_rollback_sql_test` and `production_owned_rollback_test`; network selects
`owned_rollback_sql_tcp_test` and `production_owned_rollback_crash_test`. The last
target also tests default server startup recovery, not just TCP transport.

These CTest modes reuse executables; build the right-hand target, not the mode name:

| CTest name | Build target |
| --- | --- |
| `owned_rollback_cleanup_crash_test` | `owned_rollback_cleanup_test` |
| `owned_rollback_sql_tcp_test` | `owned_rollback_sql_test` |
| `production_owned_rollback_crash_test` | `production_owned_rollback_test` |

The helper preserves the existing frame-retirement aliases as well. The longer
`owned_rollback_model_test` and `owned_rollback_sql_crash_test` stay in FULL, not
FAST/FOCUSED. If investigating them separately, use this explicit selection;
do not repeat it when the same configuration was already covered by FULL:

```sh
cmake --build build-retirement-final --parallel 2 --target owned_rollback_model_test owned_rollback_sql_test
ctest --test-dir build-retirement-final --parallel 1 --output-on-failure --no-tests=error \
  -R '^(owned_rollback_model_test|owned_rollback_sql_crash_test)$'
```

## FULL

Run once on the final source candidate before merging. This is the complete
registered Release regression: currently **103 targets**, including all model modes,
the embedded page-reclaim model, and existing benchmark correctness tests.
Those registered tests run once; the helper launches no additional benchmarks.

```sh
python3 scripts/dev-verify.py --profile FULL --area all --build-dir build-retirement-final --jobs 6 --reason "final merge candidate"
```

A failed command stops the helper; a failed FULL blocks merging. Do not claim a
pass for changed source or silently hide failures with rerun-only-failed results.
Conversely, do not rerun individual model binaries merely because FULL already
covered them. Documentation-only follow-ups need their relevant checks, not a
new clean database build; record exactly which source/configuration was tested.

## RELEASE

This profile **prints a manual checklist only**, returns 0 for successful display,
and performs no builds/tests/logging. It is never proof of a completed release:

```sh
python3 scripts/dev-verify.py --profile RELEASE --area all --build-dir build-retirement-final
```

Run FULL above, then appropriate additional configurations. The following exact
commands cover storage/recovery and reader concurrency; choose additional tests
when the change affects other areas. Instrumented model runs verify a different
configuration and are not redundant normal-Release executions.

```sh
# Reuse separate sanitizer directories; never combine ASan and TSan.
cmake -S . -B build-retirement-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-retirement-asan --parallel 2 --target \
  frame_wal_retirement_test physical_writeback_test reclaim_lineage_test \
  explicit_recovery_model_test restartable_undo_test runtime_clr_rollback_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-retirement-asan --parallel 1 --output-on-failure --no-tests=error \
  -R '^(frame_wal_retirement(_model)?_test|physical_writeback(_model)?_test|reclaim_lineage(_model)?_test|explicit_recovery_model_test|restartable_undo_test|runtime_clr_rollback_test)$'

cmake -S . -B build-retirement-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-retirement-tsan --parallel 2 --target \
  buffer_concurrency_test concurrent_sessions_test concurrent_tcp_test \
  database_access_gate_test frame_wal_retirement_test
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-retirement-tsan --parallel 1 --output-on-failure --no-tests=error \
  -R '^(buffer_concurrency_test|concurrent_sessions_test|concurrent_tcp_test|database_access_gate_test|frame_wal_retirement_concurrency_test)$'

# Explicit compatibility smoke; already included in FULL.
ctest --test-dir build-retirement-final --parallel 1 --output-on-failure --no-tests=error \
  -R '^(legacy_wal_migration_test|database_format_test|page_lsn_migration_crash_test|wal_migration_crash_test)$'
```

The compatibility selector identifies FULL coverage; do not repeat it on the same
candidate unless testing a different build/environment or investigating a failure.
Leak detection is disabled in the shown macOS ASan configuration; report that
limitation rather than claiming LSan coverage. Honor failures and unsupported
sanitizer/platform conditions; never claim checks that were not executed.

Keep `-Wall -Wextra -Wpedantic` enabled. Verify AppleClang locally and Linux/GCC
through the existing CI Release job; a second local GCC suite is optional unless
compiler portability changed. CI supplies a fresh checkout/configure/build.
Do not delete local caches just to duplicate it. For an actual release, require
the candidate's Linux CI success and an independent fresh-checkout compatibility
smoke. Existing release-tag/branch safety rules still apply.

## Local metrics and failure behavior

Every executed configure/build/CTest command appends one record to the single
gitignored repository-root `.dev-verify.jsonl`. It contains `profile`, `area`,
`command` (argv array), `elapsed_seconds` (monotonic wall time), `exit_code`,
`git_head`, `git_dirty`, and optional `reason`. Git state is captured at invocation
start, including untracked source files. No environment values, command output,
credentials, passing-test cache, or durable database state is stored. Do not put
secrets in `--reason` or filesystem paths. The log is not a proof of verification
for another dirty source tree sharing the same HEAD.

CTest inventory queries are logged too. The helper stops on the first failed
command and preserves ordinary subprocess exit codes. Missing commands return
127, other launch failures 126, and signal deaths use shell-style 128+signal
(the log retains the negative subprocess code); Ctrl-C returns 130. Logging
failure is printed and fails an otherwise successful command, but never replaces
an existing command failure with success or a different code. Argument/setup
errors return 2. Dry runs, help, and the RELEASE checklist do not write metrics.

## Maintaining this helper

Validate syntax without creating Python bytecode, and run its standard-library
unit tests (not registered as new CTest database targets):

```sh
python3 -c "from pathlib import Path; p=Path('scripts/dev-verify.py'); compile(p.read_text(), str(p), 'exec')"
python3 scripts/dev-verify.py --help
python3 -B -m unittest discover -s scripts -p 'test_dev_verify.py' -v
```

Check `--dry-run` and the relevant profiles when changing selections. Keep exact
FULL coverage and one serial CTest invocation. Ninja/ccache trials, parallel test
isolation, CI changes, and splitting embedded models are separate optimizations,
not part of this workflow helper.
