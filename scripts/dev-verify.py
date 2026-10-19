#!/usr/bin/env python3
"""Incremental, serial verification. See docs/development-workflow.md."""

import argparse
import json
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAST = {
    "buffer-wal": ("buffer_pool_manager_test", "buffer_pool_wal_test", "dirty_page_table_test"),
    "allocator-recovery": (
        "page_allocator_test", "wal_codec_test", "recovery_log_test", "clr_codec_test",
        "checkpoint_codec_test", "fuzzy_checkpoint_codec_test", "page_lsn_test",
    ),
    "sql-parser": ("sql_lexer_test", "sql_parser_test", "sql_parser_robustness_test", "sql_semantics_test"),
    "network": ("wire_protocol_test", "tcp_transport_test"),
}
INTEGRATION = {
    "buffer-wal": (
        "buffer_engine_integration_test", "frame_wal_retirement_test", "frame_wal_retirement_original_test",
        "frame_wal_retirement_concurrency_test", "physical_writeback_test", "reclaim_lineage_test",
        "runtime_clr_rollback_test", "restartable_undo_test", "fuzzy_checkpoint_test",
        "buffer_concurrency_test", "concurrent_sessions_test",
    ),
    "allocator-recovery": (
        "page_reclaim_test", "page_reservation_test", "durable_page_append_test",
        "durable_page_append_crash_test", "recovery_test", "restartable_undo_test",
        "runtime_clr_rollback_test", "multi_transaction_recovery_test", "reclaim_lineage_test",
        "checkpoint_test", "fuzzy_checkpoint_test", "segmented_wal_recovery_test",
        "page_lsn_recovery_test", "legacy_wal_migration_test", "explicit_transaction_test",
    ),
    "sql-parser": (
        "sql_executor_test", "sql_executor_random_test", "schema_tuple_test", "table_test",
        "catalog_test", "explicit_transaction_test", "transaction_recovery_test",
    ),
    "network": (
        "network_protocol_test", "network_stress_test", "concurrent_tcp_test",
        "concurrent_sessions_test", "explicit_tcp_transaction_test", "transaction_recovery_test",
    ),
}
# CTest names are not always executable/build-target names.
ALIASES = {
    "frame_wal_retirement_original_test": "reclaim_lineage_test",
    "frame_wal_retirement_concurrency_test": "frame_wal_retirement_test",
}


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, choices=("FAST", "FOCUSED", "FULL", "RELEASE"))
    parser.add_argument("--area", required=True, choices=(*FAST, "all"))
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="Single-configuration CMake directory; relative paths start at repository root")
    parser.add_argument("--jobs", type=int, default=2, help="Build jobs only; CTest is always serial (default: 2)")
    parser.add_argument("--reason", help="Optional non-sensitive explanation recorded in the local log")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan without running commands or writing logs")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if (args.profile in ("FULL", "RELEASE")) != (args.area == "all"):
        parser.error("FULL/RELEASE require --area all; FAST/FOCUSED require a specific area")
    args.build_dir = (ROOT / args.build_dir).resolve()
    if args.build_dir == ROOT or args.build_dir in ROOT.parents:
        parser.error("--build-dir must not be the source directory or its ancestor")
    return args


def check_cache(args):
    cache = args.build_dir / "CMakeCache.txt"
    if not cache.exists():
        return False
    entries = {}
    for line in cache.read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            entries[key.split(":", 1)[0]] = value
    if Path(entries.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT:
        raise ValueError("Build cache belongs to another source tree; choose its matching directory")
    if entries.get("CMAKE_CONFIGURATION_TYPES"):
        raise ValueError("Use a single-configuration Makefiles or Ninja build directory")
    if args.profile == "FULL" and entries.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("FULL requires a Release cache; use a separate Release build directory")
    return True


def selected_tests(args):
    if args.profile == "FULL":
        return ()  # Deliberately unfiltered: includes every registered model/benchmark test.
    return FAST[args.area] + (INTEGRATION[args.area] if args.profile == "FOCUSED" else ())


def commands(args, configured):
    tests = selected_tests(args)
    result = []
    if not configured:
        result.append(["cmake", "-S", str(ROOT), "-B", str(args.build_dir),
                       "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic"])
    build = ["cmake", "--build", str(args.build_dir), "--parallel", str(args.jobs)]
    if tests:
        build += ["--target", *sorted({ALIASES.get(name, name) for name in tests})]
    result.append(build)
    result.append(["ctest", "--test-dir", str(args.build_dir), "--show-only=json-v1"])
    run = ["ctest", "--test-dir", str(args.build_dir), "--parallel", "1",
           "--output-on-failure", "--no-tests=error"]
    if tests:
        run += ["-R", "^(" + "|".join(re.escape(name) for name in tests) + ")$"]
    result.append(run)
    return result


def git_state():
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    return {"git_head": head, "git_dirty": bool(dirty)}


def run_command(args, command, state, capture=False):
    print("+ " + shlex.join(command), flush=True)
    started = time.perf_counter()
    output = ""
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE if capture else None)
        code, output = result.returncode, result.stdout or ""
    except FileNotFoundError as error:
        print(error, file=sys.stderr)
        code = 127
    except OSError as error:
        print(error, file=sys.stderr)
        code = 126
    except KeyboardInterrupt:
        code = 130
    elapsed = time.perf_counter() - started
    entry = {"profile": args.profile, "area": args.area, "command": command,
             "elapsed_seconds": elapsed, "exit_code": code, **state}
    if args.reason:
        entry["reason"] = args.reason
    try:
        with (ROOT / ".dev-verify.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(entry, ensure_ascii=True) + "\n")
    except OSError as error:
        print(f"Could not record workflow metrics: {error}", file=sys.stderr)
        if code == 0:
            code = 1
    print(f"  elapsed={elapsed:.3f}s exit={code}", flush=True)
    # Keep ordinary exit codes; use shell-conventional 128+signal for signal deaths.
    return (128 - code if code < 0 else code), output


def main(argv=None):
    args = arguments(argv)
    if args.profile == "RELEASE":
        print("RELEASE is a MANUAL CHECKLIST, not a verification success; no commands were executed.")
        print("Run FULL, separate ASan/UBSan and TSan checks, compiler/compatibility checks and Linux CI.")
        print("Exact commands: " + str(ROOT / "docs/development-workflow.md") + "#release")
        return 0
    try:
        plan = commands(args, check_cache(args))
        if args.dry_run:
            for command in plan:
                print("+ " + shlex.join(command))
            print("Dry run only: no commands executed, files created, or metrics written.")
            return 0
        started = time.perf_counter()
        state = git_state()
        for command in plan:
            query = "--show-only=json-v1" in command
            code, output = run_command(args, command, state, capture=query)
            if code:
                return code
            if command[:2] == ["cmake", "--build"] and not check_cache(args):
                raise ValueError("Successful build did not produce a CMake cache")
            if query:
                registered = {item["name"] for item in json.loads(output)["tests"]}
                missing = set(selected_tests(args)) - registered
                if not registered or missing:
                    raise ValueError("CTest selection is empty or missing registered tests: " + ", ".join(sorted(missing)))
                print(f"Selected {len(selected_tests(args)) or len(registered)} of {len(registered)} registered tests.")
        print(f"{args.profile}/{args.area} passed in {time.perf_counter() - started:.3f}s", flush=True)
        return 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Verification setup failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
