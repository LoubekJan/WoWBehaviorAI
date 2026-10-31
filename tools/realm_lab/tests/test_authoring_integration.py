"""Empty-authoring integration guards; no Docker daemon or real DB is used."""
from contextlib import redirect_stdout
from dataclasses import replace
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import graveyard, manage, promotion, run_scoped, world_setup
from tools.realm_lab import settlement_population as settlement
from tools.realm_lab.tests.test_setup import environment


ROOT = Path(__file__).resolve().parents[3]
PROFILE = "authoring-empty"


class AuthoringIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.settings = replace(manage.Settings.load({**environment(), "LAB_AI_PROFILE": settlement.PROFILE}),
                                ai_profile=PROFILE)
        self.population = settlement.Population.load()

    def test_authoring_registration_is_scoped_to_realm_two(self):
        world_setup.validate_profile(self.settings)
        with self.assertRaises(manage.SetupError):
            world_setup.validate_profile(replace(self.settings, realm_id=3))
        with self.assertRaises(manage.SetupError):
            world_setup.validate_profile(replace(self.settings, ai_profile="disabled"))

    def test_schema_preparation_refuses_uncleared_world_before_any_binary_or_sql_write(self):
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "authoring_preflight", side_effect=manage.SetupError("not empty")) as empty, \
             patch.object(manage, "lab_mysql") as sql, \
             patch.object(world_setup.subprocess, "run") as binary:
            with self.assertRaisesRegex(manage.SetupError, "not empty"):
                world_setup.prepare(Path("data"), Path("template"), Path("server"), self.settings)
        empty.assert_called_once_with(Path("data"), self.settings)
        sql.assert_not_called()
        binary.assert_not_called()

    def test_schema_preparation_rechecks_empty_state_after_database_updates(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "schema.conf"
            native_path = Path
            with patch.object(world_setup, "Path", side_effect=lambda value: target if str(value) == "/tmp/world-schema-setup.conf" else native_path(value)), \
                 patch.object(settlement, "ensure_world_offline"), \
                 patch.object(manage, "authoring_preflight") as empty, \
                 patch.object(settlement, "verify_data", return_value=self.population), \
                 patch.object(manage, "preflight"), \
                 patch.object(world_setup, "demote_retired_pilots"), \
                 patch.object(world_setup.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as binary, \
                 redirect_stdout(io.StringIO()):
                world_setup.prepare(Path(directory), ROOT / "deploy/worldserver.conf", Path("server"), self.settings)
            self.assertEqual(empty.call_count, 2)
            self.assertIn("--update-databases-only", binary.call_args.args[0])
            rendered = target.read_text(encoding="utf-8")
            self.assertIn("AIWorld.Enable = 0\n", rendered)
            self.assertIn("AIWorld.TelemetryEnabled = 0\n", rendered)

    def test_start_policy_refuses_uncleared_world_without_receipt_or_sql(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / "start.json"
            with patch.object(settlement, "ensure_world_offline"), \
                 patch.object(manage, "authoring_preflight", side_effect=manage.SetupError("not empty")), \
                 patch.object(manage, "lab_mysql") as sql:
                with self.assertRaisesRegex(manage.SetupError, "not empty"):
                    world_setup.apply_start(Path(directory), receipt, self.settings)
            self.assertFalse(receipt.exists())
            sql.assert_not_called()

    def test_observer_uses_current_preview_bounds_without_loading_a_population(self):
        captured = {}

        def execute(program, arguments):
            captured.update(os.environ)
            captured["command"] = (program, arguments)

        with patch.dict(os.environ, {"LAB_AI_PROFILE": PROFILE}, clear=True), \
             patch("sys.argv", ["run_scoped.py", "uvicorn", "app.main:app"]), \
             patch.object(run_scoped.os, "execvp", side_effect=execute), \
             patch.object(run_scoped.Scope, "load", side_effect=AssertionError("old single-return scope must not load")):
            run_scoped.main()
        self.assertEqual(captured["WORLD_VIEWER_SCOPE_MAP_ID"], "725")
        self.assertEqual(captured["WORLD_VIEWER_SCOPE_ZONE_IDS"], "4988")
        self.assertEqual(json.loads(captured["WORLD_VIEWER_SCOPE_BOUNDS"]), settlement.BOUNDS)
        self.assertEqual(captured["command"], ("uvicorn", ["uvicorn", "app.main:app"]))

    def test_graveyard_setup_keeps_its_non_creature_link_in_authoring_mode(self):
        location = graveyard.Graveyard.load()
        row = dict(id=location.safe_location_id, zone=location.zone_id, team=location.team, comment=location.comment)
        responses = [json.dumps(row), "", json.dumps(row)]
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(graveyard, "verify_data", return_value=location), \
             patch.object(manage, "lab_mysql", side_effect=responses) as sql:
            result = graveyard.bootstrap(Path("data"), self.settings)
        self.assertFalse(result["spirit_healer_spawned"])
        self.assertEqual(sql.call_count, 3)
        self.assertNotIn("creature", sql.call_args_list[1].args[1])

    def test_adoption_checks_empty_state_in_container_without_using_old_population_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".env").write_text("LAB_AI_PROFILE=authoring-empty\n", encoding="utf-8")
            state = root / "receipt.json"
            containers = {}
            for service, volume in (("auth-mysql", "aitc_auth_mysql-data"), ("lab-mysql", "aitc_lab_mysql-data")):
                containers[service] = dict(State=dict(Running=True),
                    Config=dict(Labels={"com.docker.compose.project": "aitc-lab", "com.docker.compose.service": service}),
                    Mounts=[dict(Destination="/var/lib/mysql", Type="volume", Name=volume)])

            def commands(arguments, **kwargs):
                if arguments[:len(promotion.COMPOSE) + 2] == promotion.COMPOSE + ["ps", "-q"]:
                    return arguments[-1].encode()
                self.assertEqual(arguments, promotion.COMPOSE + ["run", "--rm", "--no-deps", "-T", "--interactive=false", "tc-dev",
                    "python3", "tools/realm_lab/manage.py", "preflight-authoring-empty", "/workspace/runtime/lab/data"])
                return b"Empty authoring world verified."

            with patch.object(promotion, "ROOT", root), patch.object(promotion, "STATE", state), \
                 patch.object(manage.Settings, "load", return_value=self.settings), \
                 patch.object(promotion, "run", side_effect=commands) as run, \
                 patch.object(promotion, "inspect", side_effect=lambda value: containers[value]), \
                 patch.object(promotion, "sql", return_value="2") as sql, \
                 patch.object(promotion, "account_identity", return_value={"count": 1}), \
                 patch.object(settlement, "verify_data", return_value=self.population), \
                 patch.object(settlement, "snapshot_sql", side_effect=AssertionError("old population must not be inspected")), \
                 redirect_stdout(io.StringIO()):
                promotion.adopt_canonical_installation()
            self.assertEqual(run.call_count, 3)
            sql.assert_called_once()
            receipt = json.loads(state.read_text(encoding="utf-8"))
            self.assertEqual((receipt["native_spawn_count"], receipt["managed_spawn_count"]), (0, 0))


def bash_binary():
    candidates = (["C:/Program Files/Git/bin/bash.exe", shutil.which("bash")] if os.name == "nt"
                  else [shutil.which("bash")])
    return next((value for value in candidates if value and Path(value).is_file()), None)


@unittest.skipUnless(bash_binary(), "Bash is required for actual deployment-flow checks")
class AuthoringShellIntegrationTests(unittest.TestCase):
    HARNESS = r'''
set -euo pipefail
WORLD_PRESENT=1
flock() { :; }
python3() { printf 'test-uuid'; }
make() { printf 'make %s\n' "$*" >> "$CALLS"; }
git() {
  if [ "$*" = 'branch --show-current' ]; then printf 'AI-World-lab'; else printf '%s' "$REVISION"; fi
}
docker() {
  printf 'docker %s\n' "$*" >> "$CALLS"
  case "$*" in
    *'print(Settings.load(os.environ).ai_profile)'*) printf 'authoring-empty';;
    *'preflight-authoring-empty'*) if [ "$FAIL_PREFLIGHT" = 1 ]; then return 42; fi;;
    *'ps --all -q worldserver'*) if [ "$WORLD_PRESENT" = 1 ]; then printf 'test-world'; fi;;
    'inspect --format {{.State.Running}} test-world') printf 'false';;
    'rm test-world') WORLD_PRESENT=0;;
    *'ps -q '*) printf 'test-service';;
    'inspect --format {{.State.Health.Status}} test-service') printf 'healthy';;
    *'/build/bin/worldserver --version'*) printf 'TrinityCore rev. %s' "${REVISION:0:12}";;
  esac
}
set -- "$REVISION"
source "$SCRIPT"
'''

    def run_script(self, filename, fail=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".env").write_text("", encoding="utf-8")
            calls = root / "calls.txt"
            env = {**os.environ, "SCRIPT": (ROOT / "docker/scripts" / filename).as_posix(),
                   "CALLS": calls.as_posix(), "FAIL_PREFLIGHT": str(int(fail)), "REVISION": "a" * 40}
            # Binary stdin prevents Windows text-mode conversion to CRLF.
            result = subprocess.run([bash_binary(), "--noprofile", "--norc", "-s"], input=self.HARNESS.encode("utf-8"),
                capture_output=True, cwd=root, env=env, timeout=20)
            return result, calls.read_text(encoding="utf-8").splitlines() if calls.exists() else []

    def test_setup_and_deploy_do_not_repopulate_or_apply_templates(self):
        for filename in ("setup-world.sh", "deploy-lab.sh"):
            with self.subTest(script=filename):
                result, calls = self.run_script(filename)
                self.assertEqual(result.returncode, 0, result.stderr[-2000:])
                checks = [index for index, call in enumerate(calls) if "preflight-authoring-empty" in call]
                stops = [index for index, call in enumerate(calls) if "stop worldserver" in call]
                self.assertGreaterEqual(len(checks), 2)
                self.assertLess(checks[0], stops[0])
                self.assertTrue(all("bootstrap-" not in call and "character_templates" not in call and
                                    "telemetry_contract.py" not in call for call in calls))
                self.assertTrue(any("world_setup.py prepare" in call for call in calls))

    def test_uncleared_world_refuses_both_flows_before_stop_or_preparation(self):
        for filename in ("setup-world.sh", "deploy-lab.sh"):
            with self.subTest(script=filename):
                result, calls = self.run_script(filename, fail=True)
                self.assertEqual(result.returncode, 42, result.stderr[-2000:])
                self.assertTrue(any("preflight-authoring-empty" in call for call in calls))
                self.assertTrue(all("stop worldserver" not in call and "world_setup.py prepare" not in call and
                                    "bootstrap-" not in call for call in calls))


if __name__ == "__main__":
    unittest.main()
