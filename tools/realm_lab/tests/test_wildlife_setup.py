"""Runtime/profile integration keeps ownership opt-in and reuses canonical setup."""
from dataclasses import replace
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, settlement_population as settlement, wildlife_population as wildlife, world_setup
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_wildlife_population import snapshot


class WildlifeSetupTests(unittest.TestCase):
    def setUp(self):
        self.settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": wildlife.PROFILE})
        self.population = wildlife.Population.load()

    def test_active_and_paused_configs_preserve_exact_scope_and_disable_other_owners(self):
        active = manage.config_overrides(self.settings, self.population)
        for key in ("AIWorld.Enable", "AIWorld.LivingRolesEnabled", "AIWorld.LivingRoleExtensionsEnabled",
                    "AIWorld.LivingNeedEvolutionEnabled", "AIWorld.TelemetryNativeCreaturesEnabled"):
            self.assertEqual(active[key], "1")
        for key in ("AIWorld.EnableSpawnReconciliation", "AIWorld.EnableZoneControlActivation",
                    "AIWorld.RemoteInferenceEnabled", "AIWorld.RecoveryAdviceEnabled", "AIWorld.DynamicTaskEnable",
                    "AIWorld.CoalitionMaintenance", "AIWorld.GroupCoordination", "AIWorld.LivingWolvesEnabled"):
            self.assertEqual(active[key], "0")
        self.assertEqual(active["AIWorld.ScopeSpawnIds"], self.population.settlement.config()["AIWorld.ScopeSpawnIds"])
        paused = manage.config_overrides(replace(self.settings, wildlife_enabled=False), self.population)
        self.assertEqual(paused["AIWorld.Enable"], "0")
        self.assertEqual(paused["AIWorld.TelemetryNativeCreaturesEnabled"], "1")
        self.assertEqual(paused["AIWorld.ScopeSpawnIds"], active["AIWorld.ScopeSpawnIds"])
        with self.assertRaises(manage.SetupError):
            manage.config_overrides(self.settings, self.population.settlement)

    def test_pause_flag_is_numeric_and_does_not_change_saved_ownership(self):
        self.assertFalse(manage.Settings.load({**environment(), "LAB_AI_PROFILE": wildlife.PROFILE,
                                              "LAB_WILDLIFE_ENABLED": "0"}).wildlife_enabled)
        for invalid in ("true", "2", "1;bad"):
            with self.subTest(value=invalid), self.assertRaises(manage.SetupError):
                manage.Settings.load({**environment(), "LAB_WILDLIFE_ENABLED": invalid})

    def test_offline_bootstrap_builds_missing_native_spawns_before_activating_only_animals(self):
        disabled = snapshot(self.population, control=0, legacy=False)
        enabled = snapshot(self.population, control=1, legacy=False)
        queries = []
        def sql(settings, statement):
            queries.append(statement)
            if statement.lstrip().startswith("SELECT JSON_OBJECT"):
                import json
                selected = enabled if any("@wildlife_population_ready" in q for q in queries[:-1]) else disabled
                return "\n".join(json.dumps(row) for row in selected)
            return ""
        with patch.object(wildlife, "verify_data", return_value=self.population), \
             patch.object(settlement, "ensure_world_offline") as guard, \
             patch.object(manage, "lab_mysql", side_effect=sql):
            manage.bootstrap_wildlife(Path("data"), self.settings)
        self.assertGreaterEqual(guard.call_count, 3)
        native = next(i for i, q in enumerate(queries) if "INSERT INTO world.creature (" in q)
        agents = next(i for i, q in enumerate(queries) if "INSERT INTO characters.ai_agents" in q)
        activation = next(i for i, q in enumerate(queries) if "@wildlife_population_ready" in q)
        self.assertLess(native, agents)
        self.assertLess(agents, activation)
        self.assertTrue(all("DELETE" not in q and "SET control_mode=0" not in q for q in queries))

    def test_repeated_schema_setup_allows_selected_animals_but_no_foreign_ai(self):
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(self.settings)
        query = sql.call_args_list[-1].args[1]
        self.assertIn("agent_id NOT IN (901072,", query)
        self.assertIn("spawn_id<>agent_id OR map_id<>725", query)
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "1"]):
            with self.assertRaises(manage.SetupError):
                world_setup.demote_retired_pilots(self.settings)


if __name__ == "__main__":
    unittest.main()
