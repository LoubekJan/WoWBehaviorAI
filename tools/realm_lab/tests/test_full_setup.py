"""Full-world rollout boundaries without a Docker daemon or production writes."""
from contextlib import redirect_stdout
from dataclasses import replace
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import (defias_population as defias, full_population as full,
    graveyard, manage, promotion, run_scoped, settlement_population as settlement, world_setup)
from tools.realm_lab.tests import test_full_population as full_tests
from tools.realm_lab.tests.test_setup import environment


class FullSetupTests(unittest.TestCase):
    def setUp(self):
        self.population = full.Population.load()
        self.settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "full-v8",
                                              "LAB_NPC_AGENTS_ENABLED": "1"})

    def test_explicit_full_profile_runs_all_roles_but_keeps_other_decision_owners_off(self):
        self.assertEqual(manage.Settings.load(environment()).ai_profile, "disabled")
        config = manage.config_overrides(self.settings, self.population)
        for flag in ("Enable", "LivingRolesEnabled", "LivingRoleExtensionsEnabled", "LivingNeedEvolutionEnabled",
                     "LivingSupplyEnabled", "LivingConsumersEnabled", "LivingRaidsEnabled", "LivingFieldsEnabled",
                     "LivingDefias.Enabled", "NpcAgentEnabled", "ScopeRestrictAgents", "ScopeAlwaysActive"):
            self.assertEqual(config["AIWorld." + flag], "1", flag)
        self.assertEqual(config["AIWorld.ScopeMapId"], "725")
        scoped = {int(value) for value in config["AIWorld.ScopeSpawnIds"].strip('"').split(",")}
        self.assertEqual(scoped, set(range(901000, 901100)))
        for flag in ("EnableSpawnReconciliation", "EnableZoneControlActivation", "RemoteInferenceEnabled",
                     "CoalitionMaintenance", "GroupCoordination", "DefiasGroupAutoFormation", "LivingWolvesEnabled"):
            self.assertEqual(config["AIWorld." + flag], "0", flag)
        for existing in ("defias-v8", "fields-v8", "raids-v8"):
            self.assertEqual(manage.Settings.load({**environment(), "LAB_AI_PROFILE": existing}).ai_profile, existing)

    def test_pause_keeps_scope_and_observation_without_runtime_or_llm_actions(self):
        active = manage.config_overrides(self.settings, self.population)
        paused = manage.config_overrides(replace(self.settings, wildlife_enabled=False), self.population)
        self.assertEqual(paused["AIWorld.ScopeSpawnIds"], active["AIWorld.ScopeSpawnIds"])
        self.assertEqual(paused["AIWorld.TelemetryEnabled"], "1")
        self.assertEqual(paused["AIWorld.TelemetryNativeCreaturesEnabled"], "1")
        for flag in ("Enable", "LivingRolesEnabled", "LivingRoleExtensionsEnabled", "LivingNeedEvolutionEnabled",
                     "LivingSupplyEnabled", "LivingConsumersEnabled", "LivingRaidsEnabled", "LivingFieldsEnabled",
                     "LivingDefias.Enabled", "NpcAgentEnabled"):
            self.assertEqual(paused["AIWorld." + flag], "0", flag)

    def test_full_scope_cannot_be_substituted_with_a_partial_or_foreign_profile(self):
        for wrong in (self.population.defias_population, self.population.settlement):
            with self.subTest(profile=wrong.profile), self.assertRaises(manage.SetupError):
                manage.config_overrides(self.settings, wrong)
        world_setup.validate_profile(self.settings)
        with self.assertRaises(manage.SetupError):
            world_setup.validate_profile(replace(self.settings, realm_id=3))
        for override in ({"LAB_REALM_ID": "3"}, {"LAB_AI_PROFILE": "full-v9"}):
            with self.subTest(override=override), self.assertRaises(manage.SetupError):
                manage.Settings.load({**environment(), "LAB_AI_PROFILE": "full-v8", **override})

    def _bootstrap_queries(self, *, missing_before_activation=False, activation_succeeds=True):
        def current_rows(control):
            # Production no longer contains the retired 900725 test cohort.
            # The reusable fixture includes that synthetic history by default.
            return [row for row in full_tests.snapshot(self.population, control=control)
                    if not any(row.get(key) in settlement.LEGACY_IDS for key in ("id", "spawn", "guid", "member"))]
        disabled = current_rows(0)
        enabled = current_rows(1)
        reads = 0
        queries = []
        activation = full.activation_sql(self.population)

        def sql(settings, statement):
            nonlocal reads
            queries.append(statement)
            if statement.lstrip().startswith("SELECT JSON_OBJECT"):
                reads += 1
                rows = enabled if activation_succeeds and reads >= 3 else disabled
                if missing_before_activation and reads == 2:
                    rows = [row for row in rows if not (row["kind"] == "agent" and row.get("id") == 901014)]
                return "\n".join(json.dumps(row) for row in rows)
            return ""
        return queries, activation, sql

    def test_offline_bootstrap_builds_native_spawns_and_reservations_before_checked_activation(self):
        queries, activation, sql = self._bootstrap_queries()
        with patch.object(full, "verify_data", return_value=self.population), \
             patch.object(settlement, "ensure_world_offline") as offline, \
             patch.object(manage, "lab_mysql", side_effect=sql), redirect_stdout(io.StringIO()) as output:
            manage.bootstrap_full(Path("data"), self.settings)
        self.assertGreaterEqual(offline.call_count, 3)
        native = next(i for i, query in enumerate(queries) if "INSERT INTO world.creature (" in query)
        reservation = next(i for i, query in enumerate(queries) if "INSERT INTO characters.ai_agents" in query)
        activated = queries.index(activation)
        self.assertLess(native, reservation)
        self.assertLess(reservation, activated)
        self.assertTrue(queries[-1].lstrip().startswith("SELECT JSON_OBJECT"))
        self.assertIn("ownership verified", output.getvalue())
        self.assertTrue(all("DELETE" not in query and "SET control_mode=0" not in query for query in queries))

    def test_live_world_or_missing_recruit_readback_prevents_activation(self):
        with patch.object(settlement, "ensure_world_offline", side_effect=full.ProfileError("world is running")), \
             patch.object(full, "verify_data") as data, patch.object(manage, "lab_mysql") as sql:
            with self.assertRaises(full.ProfileError):
                manage.bootstrap_full(Path("data"), self.settings)
        data.assert_not_called()
        sql.assert_not_called()
        queries, activation, sql = self._bootstrap_queries(missing_before_activation=True)
        with patch.object(full, "verify_data", return_value=self.population), \
             patch.object(settlement, "ensure_world_offline"), patch.object(manage, "lab_mysql", side_effect=sql):
            with self.assertRaises(full.ProfileError):
                manage.bootstrap_full(Path("data"), self.settings)
        self.assertFalse(any(query == activation for query in queries), "Activation ran without complete recruit read-back")

    def test_unconfirmed_activation_cannot_report_success(self):
        queries, activation, sql = self._bootstrap_queries(activation_succeeds=False)
        with patch.object(full, "verify_data", return_value=self.population), \
             patch.object(settlement, "ensure_world_offline"), patch.object(manage, "lab_mysql", side_effect=sql), \
             redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(full.ProfileError):
                manage.bootstrap_full(Path("data"), self.settings)
        self.assertTrue(any(query == activation for query in queries), "Activation was not attempted before read-back failure")
        self.assertNotIn("ownership verified", output.getvalue())

    def test_schema_setup_accepts_all_hundred_but_refuses_foreign_managed_identity(self):
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(self.settings)
        check = sql.call_args_list[-1].args[1]
        self.assertIn("agent_id NOT IN (" + ",".join(str(i) for i in range(901000, 901100)) + ")", check)
        self.assertIn("spawn_id<>agent_id OR map_id<>725", check)
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "1"]):
            with self.assertRaisesRegex(manage.SetupError, "Unexpected managed"):
                world_setup.demote_retired_pilots(self.settings)

    def test_observer_and_recorder_use_the_same_reviewed_custom_map(self):
        captured = {}
        def execvp(command, args):
            captured.update(command=command, args=args, scope={key: value for key, value in os.environ.items()
                if key.startswith("WORLD_VIEWER_SCOPE_")})
        with patch.dict(os.environ, {"LAB_AI_PROFILE": "full-v8"}, clear=True), \
             patch.object(run_scoped.sys, "argv", ["run_scoped.py", "observer-command", "--check"]), \
             patch.object(run_scoped.os, "execvp", side_effect=execvp):
            run_scoped.main()
        self.assertEqual(captured["command"], "observer-command")
        self.assertEqual(captured["args"], ["observer-command", "--check"])
        self.assertEqual(captured["scope"]["WORLD_VIEWER_SCOPE_MAP_ID"], "725")
        self.assertEqual(captured["scope"]["WORLD_VIEWER_SCOPE_ZONE_IDS"], "4988")
        self.assertEqual(json.loads(captured["scope"]["WORLD_VIEWER_SCOPE_BOUNDS"]), settlement.BOUNDS)

    def test_deploy_profile_commands_reach_full_preflight_bootstrap_and_pause(self):
        for command, function in (("preflight-full-v8", "full_preflight"),
                                  ("bootstrap-full-v8", "bootstrap_full"),
                                  ("deactivate-full-v8", "deactivate_full")):
            with self.subTest(command=command), patch.object(manage.Settings, "load", return_value=self.settings), \
                 patch.object(manage.sys, "argv", ["manage.py", command, "runtime/data"]), \
                 patch.object(manage, function) as action:
                manage.main()
            action.assert_called_once_with(Path("runtime/data"), self.settings)

    def test_promotion_preserves_full_profile_and_existing_auth_credentials(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "deploy/lab/.env"
            source.parent.mkdir(parents=True)
            values = {**environment(), "LAB_REALM_ID": "2", "LAB_AI_PROFILE": "full-v8"}
            source.write_text("".join(f"{key}={value}\n" for key, value in values.items()), encoding="utf-8")
            with patch.object(promotion, "ROOT", root):
                promotion.configure_env({})
            result = promotion.read_env(root / ".env")
        self.assertEqual(result["LAB_AI_PROFILE"], "full-v8")
        self.assertEqual(result["TC_DB_PASSWORD"], values["TC_DB_PASSWORD"])
        self.assertEqual(result["TC_AUTH_DB_PASSWORD"], values["TC_AUTH_DB_PASSWORD"])
        self.assertEqual(result["TC_AUTH_DB_HOST"], "aiworld-auth-db")

    def test_graveyard_routing_is_still_verified_for_full_world(self):
        location = graveyard.Graveyard.load()
        route = dict(id=location.safe_location_id, zone=location.zone_id, team=location.team, comment=location.comment)
        with patch.object(settlement, "ensure_world_offline") as offline, \
             patch.object(graveyard, "verify_data", return_value=location), \
             patch.object(manage, "lab_mysql", side_effect=["", "", json.dumps(route)]) as sql:
            result = graveyard.bootstrap(Path("data"), self.settings, location)
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["map_id"], 725)
        self.assertEqual(offline.call_count, 3)
        self.assertEqual(sql.call_count, 3)


if __name__ == "__main__":
    unittest.main()
