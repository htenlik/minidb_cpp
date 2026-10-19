"""Standard-library tests for the workflow helper, not database CTest coverage."""

import contextlib
import importlib.util
import io
import json
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("dev_verify", Path(__file__).with_name("dev-verify.py"))
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name).resolve()
        self.root_patch = patch.object(verify, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.addCleanup(self.directory.cleanup)
        self.state = {"git_head": "test-head", "git_dirty": True}

    def args(self, profile="FAST", area="buffer-wal"):
        return verify.arguments(["--profile", profile, "--area", area, "--build-dir", "build-test"])

    def test_help_and_rejected_arguments(self):
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as caught:
            verify.arguments(["--help"])
        self.assertEqual(caught.exception.code, 0)
        for extra in (
            ["--profile", "UNKNOWN"], ["--area", "UNKNOWN"], ["--jobs", "0"],
            ["--profile", "FULL"], ["--area", "all"], ["--build-dir", str(self.root)],
        ):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    verify.arguments(["--profile", "FAST", "--area", "buffer-wal", "--build-dir", "build-test", *extra])
                self.assertEqual(caught.exception.code, 2)

    def test_profiles_and_aliases(self):
        for area in verify.FAST:
            fast = self.args(area=area)
            focused = self.args("FOCUSED", area)
            self.assertTrue(set(verify.selected_tests(fast)) < set(verify.selected_tests(focused)))
            self.assertEqual(len(verify.selected_tests(focused)), len(set(verify.selected_tests(focused))))
            plan = verify.commands(focused, True)
            self.assertEqual(plan[-1][plan[-1].index("--parallel") + 1], "1")
        self.assertNotIn("page_reclaim_test", verify.FAST["allocator-recovery"])
        self.assertIn("page_reclaim_test", verify.INTEGRATION["allocator-recovery"])
        build = verify.commands(self.args("FOCUSED"), True)[0]
        self.assertIn("reclaim_lineage_test", build)
        self.assertNotIn("frame_wal_retirement_original_test", build)
        full = verify.commands(self.args("FULL", "all"), True)
        self.assertNotIn("--target", full[0])
        self.assertNotIn("-R", full[-1])
        self.assertNotIn("-E", full[-1])
        self.assertIn("--no-tests=error", full[-1])

    def test_dry_run_is_nonmutating(self):
        with patch.object(verify.subprocess, "run", side_effect=AssertionError("Executed a dry-run command")):
            with contextlib.redirect_stdout(io.StringIO()):
                result = verify.main(["--profile", "FAST", "--area", "network", "--build-dir", "new-build", "--dry-run"])
        self.assertEqual(result, 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_release_is_manual(self):
        with patch.object(verify.subprocess, "run", side_effect=AssertionError("Executed release checklist")):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(verify.main(["--profile", "RELEASE", "--area", "all", "--build-dir", "build-test"]), 0)
        self.assertIn("not a verification success", output.getvalue())
        self.assertEqual(list(self.root.iterdir()), [])

    def test_cache_safety(self):
        args = self.args("FULL", "all")
        self.assertFalse(verify.check_cache(args))
        args.build_dir.mkdir()
        cache = args.build_dir / "CMakeCache.txt"
        for source, config in ((self.root, "Debug"), (self.root / "other", "Release")):
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\nCMAKE_BUILD_TYPE:STRING={config}\n")
            with self.assertRaises(ValueError):
                verify.check_cache(args)
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nCMAKE_BUILD_TYPE:STRING=Release\n")
        self.assertTrue(verify.check_cache(args))
        with cache.open("a") as stream:
            stream.write("CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n")
        with self.assertRaises(ValueError):
            verify.check_cache(args)

    def test_real_child_failure_propagates_and_is_logged(self):
        command = [sys.executable, "-c", "import sys; sys.exit(37)"]
        with patch.object(verify, "commands", return_value=[command]), patch.object(verify, "git_state", return_value=self.state):
            with contextlib.redirect_stdout(io.StringIO()):
                result = verify.main(["--profile", "FAST", "--area", "buffer-wal", "--build-dir", "build-test", "--reason", "failure-propagation test"])
        self.assertEqual(result, 37)
        record = json.loads((self.root / ".dev-verify.jsonl").read_text())
        self.assertEqual(record["exit_code"], 37)
        self.assertEqual(record["command"], command)
        self.assertEqual(record["git_head"], "test-head")
        self.assertTrue(record["git_dirty"])
        self.assertGreaterEqual(record["elapsed_seconds"], 0)
        self.assertEqual(record["reason"], "failure-propagation test")
        self.assertEqual(set(record), {"profile", "area", "command", "elapsed_seconds", "exit_code", "git_head", "git_dirty", "reason"})

    def test_logging_failure_never_masks_command_failure(self):
        (self.root / ".dev-verify.jsonl").mkdir()
        for child_code, expected in ((37, 37), (0, 1)):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code, _ = verify.run_command(self.args(), [sys.executable, "-c", f"raise SystemExit({child_code})"], self.state)
            self.assertEqual(code, expected)

    def test_missing_tools_and_signals(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code, _ = verify.run_command(self.args(), [str(self.root / "missing-tool")], self.state)
        self.assertEqual(code, 127)
        with patch.object(verify.subprocess, "run", return_value=subprocess.CompletedProcess([], -signal.SIGTERM)):
            with contextlib.redirect_stdout(io.StringIO()):
                code, _ = verify.run_command(self.args(), ["test-child"], self.state)
        self.assertEqual(code, 128 + signal.SIGTERM)

    def test_empty_or_missing_ctest_selection_rejected(self):
        for names in ([], ["unrelated_test"]):
            result = subprocess.CompletedProcess([], 0, json.dumps({"tests": [{"name": name} for name in names]}))
            with patch.object(verify, "check_cache", return_value=True), patch.object(verify, "git_state", return_value=self.state):
                with patch.object(verify.subprocess, "run", return_value=result):
                    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                        code = verify.main(["--profile", "FAST", "--area", "network", "--build-dir", "build-test"])
            self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
