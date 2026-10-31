"""Run the deployment scripts' real offline guard against a fake Docker CLI."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
WINDOWS_BASH = Path("C:/Program Files/Git/bin/bash.exe")
BASH = str(WINDOWS_BASH) if os.name == "nt" and WINDOWS_BASH.is_file() else shutil.which("bash")
SCRIPTS = ("deploy-lab.sh", "setup-world.sh")

STUB = r'''set -euo pipefail
lab_compose=(docker compose)
compose=(docker compose)
lab_profile=full-v8
profile=full-v8
docker() {
  case "$1" in
    compose)
      shift
      case "$1" in
        ps)
          printf 'ps\n' >> "$GUARD_EVENTS"
          if [ -f "$GUARD_STATE/exists" ]; then printf 'world-id\n'; fi
          ;;
        stop)
          printf 'stop\n' >> "$GUARD_EVENTS"
          if [ "$GUARD_SCENARIO" != still-running ]; then rm -f "$GUARD_STATE/running"; fi
          ;;
        up) printf 'up\n' >> "$GUARD_EVENTS" ;;
        run)
          # The real template restore must only execute after the stopped
          # container was removed and absence was explicitly attested.
          [ ! -f "$GUARD_STATE/exists" ] && [ ! -f "$GUARD_STATE/running" ] || return 77
          case "$*" in *"LAB_SETTLEMENT_WORLD_ABSENT=1"*"character_templates restore"*) ;; *) return 78;; esac
          printf 'restore\n' >> "$GUARD_EVENTS"
          ;;
        *) return 71 ;;
      esac
      ;;
    inspect)
      [ "$2" = --format ] && [ "$3" = '{{.State.Running}}' ] && [ "$4" = world-id ] || return 72
      printf 'inspect\n' >> "$GUARD_EVENTS"
      if [ -f "$GUARD_STATE/running" ]; then printf 'true\n'; else printf 'false\n'; fi
      ;;
    rm)
      [ "$#" = 2 ] && [ "$2" = world-id ] || return 73
      printf 'remove\n' >> "$GUARD_EVENTS"
      [ ! -f "$GUARD_STATE/running" ] || return 74
      [ "$GUARD_SCENARIO" != remove-fails ] || return 75
      if [ "$GUARD_SCENARIO" != survives ]; then rm "$GUARD_STATE/exists"; fi
      ;;
    *) return 76 ;;
  esac
}
'''


@unittest.skipUnless(BASH, "Offline deployment guards require Bash")
class OfflineContainerGuardTests(unittest.TestCase):
    def run_guard(self, script, scenario):
        source = (ROOT / "docker/scripts" / script).read_text(encoding="utf-8")
        begin = source.index('world_id="$(')
        end = source.index('if [ "$lab_profile" = settlement-v8 ]', begin) if script == "deploy-lab.sh" else source.index('receipt="', begin)
        guard = source[begin:end]
        arguments = '"${run_args[@]}"' if script == "deploy-lab.sh" else '"${run[@]}"'
        with tempfile.TemporaryDirectory(dir=ROOT) as temporary:
            directory = Path(temporary)
            if scenario != "absent":
                (directory / "exists").touch()
            if scenario in ("running", "still-running"):
                (directory / "running").touch()
            events = directory / "events.txt"
            events.touch()
            harness = directory / "guard.sh"
            harness.write_text(STUB + guard + '\nprintf "%s\\n" ' + arguments,
                               encoding="utf-8", newline="\n")
            environment = dict(os.environ, GUARD_STATE=directory.as_posix(),
                               GUARD_EVENTS=events.as_posix(), GUARD_SCENARIO=scenario)
            result = subprocess.run([BASH, harness.as_posix()], env=environment,
                                    capture_output=True, text=True, timeout=15)
            return result, events.read_text(encoding="utf-8").splitlines()

    def test_running_stopped_and_missing_containers_use_verified_absence(self):
        for script in SCRIPTS:
            for scenario in ("running", "stopped", "absent"):
                with self.subTest(script=script, scenario=scenario):
                    result, events = self.run_guard(script, scenario)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("LAB_SETTLEMENT_WORLD_ABSENT=1", result.stdout)
                    self.assertNotIn("LAB_SETTLEMENT_WORLD_IP", result.stdout)
                    self.assertEqual(events[-1], "restore")
                    self.assertLess(max(i for i, event in enumerate(events) if event == 'ps'), events.index('restore'))
                    if scenario != "absent":
                        self.assertLess(events.index("stop"), events.index("inspect"))
                        self.assertLess(events.index("inspect"), events.index("remove"))
                        self.assertLess(events.index("remove"), len(events) - 1)
                    else:
                        self.assertNotIn("remove", events)

    def test_live_failed_removal_and_surviving_containers_cannot_attest_absence(self):
        for script in SCRIPTS:
            for scenario in ("still-running", "remove-fails", "survives"):
                with self.subTest(script=script, scenario=scenario):
                    result, events = self.run_guard(script, scenario)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn("LAB_SETTLEMENT_WORLD_ABSENT=1", result.stdout)
                    self.assertNotIn('restore', events)
                    if scenario == "still-running":
                        self.assertNotIn("remove", events)


if __name__ == "__main__":
    unittest.main()
