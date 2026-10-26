"""Exercise the runner restart wrapper with isolated paths and a stub upstream."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "deploy/runner/restart-entrypoint.sh"
CONFIG_FILES = (".runner", ".runner_migrated", ".credentials", ".credentials_migrated", ".credentials_rsaparams")
WINDOWS_BASH = Path("C:/Program Files/Git/bin/bash.exe")
BASH = str(WINDOWS_BASH) if os.name == "nt" and WINDOWS_BASH.is_file() else shutil.which("bash")


@unittest.skipUnless(BASH, "Runner restart behavior requires Bash")
class RunnerRestartTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runner = self.root / "runner"
        self.work = self.root / "work"
        self.runner.mkdir()
        self.work.mkdir()
        (self.work / ".runner").write_text("workdir-marker", encoding="utf-8")
        (self.runner / ".unrelated").write_text("keep", encoding="utf-8")
        self.stub = self.root / "upstream.sh"
        self.stub.write_text("""#!/bin/bash
set -euo pipefail
[[ "$PWD" -ef "$TEST_RUNNER_DIRECTORY" ]]
[[ "${ACCESS_TOKEN:-}" == test-sentinel ]]
if [[ "${TEST_EXPECT_EMPTY:-0}" == 1 ]]; then
  for name in .runner .runner_migrated .credentials .credentials_migrated .credentials_rsaparams; do
    [[ ! -e "$PWD/$name" ]] || exit 42
  done
fi
printf '%s\n' "$@" > "$TEST_ARGUMENT_FILE"
if [[ "${TEST_CONFIGURE:-0}" == 1 ]]; then
  for name in .runner .runner_migrated .credentials .credentials_migrated .credentials_rsaparams; do
    printf 'configured\n' > "$PWD/$name"
  done
fi
exit "${TEST_EXIT_CODE:-0}"
""", encoding="utf-8", newline="\n")
        self.stub.chmod(0o755)
        self.wrapper = self.root / "restart.sh"
        # Replace only deployment-owned absolute paths. The executable body,
        # guards, argument forwarding and fixed-file cleanup stay unchanged.
        self.wrapper.write_text(SCRIPT.read_text(encoding="utf-8")
                                .replace("/actions-runner", self.runner.as_posix())
                                .replace("/entrypoint.sh", self.stub.as_posix()),
                                encoding="utf-8", newline="\n")
        self.arguments = self.root / "arguments.txt"

    def run_wrapper(self, **overrides):
        environment = dict(os.environ)
        for name in ("CONFIGURED_ACTIONS_RUNNER_FILES_DIR", "DEBUG_ONLY"):
            environment.pop(name, None)
        environment.update(ACCESS_TOKEN="test-sentinel", RUNNER_WORKDIR=self.work.as_posix(),
                           TEST_RUNNER_DIRECTORY=self.runner.as_posix(),
                           TEST_ARGUMENT_FILE=self.arguments.as_posix())
        environment.update(overrides)
        result = subprocess.run([BASH, self.wrapper.as_posix(), "./bin/Runner.Listener", "run", "argument with spaces"],
                                env=environment, capture_output=True, text=True, timeout=15)
        self.assertNotIn("test-sentinel", result.stdout + result.stderr)
        return result

    def seed_configuration(self, names=CONFIG_FILES):
        for name in names:
            (self.runner / name).write_text("old configuration", encoding="utf-8")

    def assert_unrelated_untouched(self):
        self.assertEqual((self.work / ".runner").read_text(encoding="utf-8"), "workdir-marker")
        self.assertEqual((self.runner / ".unrelated").read_text(encoding="utf-8"), "keep")

    def test_repeated_restarts_clear_complete_config_but_keep_workdir_and_forward_arguments(self):
        self.seed_configuration()
        for _ in range(2):
            result = self.run_wrapper(TEST_EXPECT_EMPTY="1", TEST_CONFIGURE="1")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.arguments.read_text(encoding="utf-8").splitlines(),
                             ["./bin/Runner.Listener", "run", "argument with spaces"])
            self.assertTrue(all((self.runner / name).is_file() for name in CONFIG_FILES))
            self.assert_unrelated_untouched()

    def test_migrated_only_configuration_cannot_block_new_registration(self):
        self.seed_configuration((".runner_migrated", ".credentials_migrated"))
        result = self.run_wrapper(TEST_EXPECT_EMPTY="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any((self.runner / name).exists() for name in CONFIG_FILES))
        self.assert_unrelated_untouched()

    def test_explicit_reuse_preserves_configuration_and_upstream_owns_reuse(self):
        self.seed_configuration()
        result = self.run_wrapper(CONFIGURED_ACTIONS_RUNNER_FILES_DIR="/existing-config")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(all((self.runner / name).read_text(encoding="utf-8") == "old configuration" for name in CONFIG_FILES))
        self.assert_unrelated_untouched()

    def test_debug_only_preserves_configuration(self):
        self.seed_configuration()
        result = self.run_wrapper(DEBUG_ONLY="true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(all((self.runner / name).exists() for name in CONFIG_FILES))
        self.assert_unrelated_untouched()

    def test_upstream_exit_status_is_returned_unchanged(self):
        self.assertEqual(self.run_wrapper(TEST_EXIT_CODE="17").returncode, 17)


class RunnerImageCommandTests(unittest.TestCase):
    def test_wrapper_entrypoint_retains_the_original_listener_command(self):
        dockerfile = (ROOT / "deploy/runner/Dockerfile").read_text(encoding="utf-8")
        entries = {line.split(" ", 1)[0]: json.loads(line.split(" ", 1)[1])
                   for line in dockerfile.splitlines() if line.startswith(("ENTRYPOINT ", "CMD "))}
        self.assertEqual(entries["ENTRYPOINT"], ["/restart-entrypoint.sh"])
        self.assertEqual(entries["CMD"], ["./bin/Runner.Listener", "run", "--startuptype", "service"])


if __name__ == "__main__":
    unittest.main()
