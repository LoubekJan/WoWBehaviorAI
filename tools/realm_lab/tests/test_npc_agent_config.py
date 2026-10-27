"""Opt-in NPC decisions preserve the reviewed lab population and legacy flags."""
from dataclasses import replace
from pathlib import Path
import unittest

import yaml

from tools.realm_lab import consumer_population, field_population, hunt_population, manage, raid_population, settlement_population
from tools.realm_lab.tests.test_setup import environment

ROOT = Path(__file__).resolve().parents[3]
NPC_DEFAULTS = {
    "AIWorld.NpcAgentEnabled": "0",
    "AIWorld.NpcAgentProfilesDirectory": '"/workspace/data/npc_agents"',
    "AIWorld.NpcAgentDecisionIntervalMs": "8000",
    "AIWorld.NpcAgentMaxInFlight": "2",
    "AIWorld.NpcAgentTimeoutMs": "15000",
}


class NpcAgentConfigurationTests(unittest.TestCase):
    def test_every_existing_profile_keeps_npc_models_disabled_by_default(self):
        profiles = ("disabled", "single-return", settlement_population.PROFILE,
            "wildlife-v8", "supply-v8", consumer_population.PROFILE, raid_population.PROFILE,
            *hunt_population.PROFILES)
        for profile in profiles:
            with self.subTest(profile=profile):
                settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile})
                overrides = manage.config_overrides(settings)
                self.assertEqual({key: overrides[key] for key in NPC_DEFAULTS}, NPC_DEFAULTS)
                self.assertEqual(overrides["AIWorld.RemoteInferenceEnabled"], "0")

    def test_only_consumers_and_raids_can_opt_in_without_changing_scope_or_other_flags(self):
        for population_type in (consumer_population.Population, raid_population.Population, field_population.Population):
            population = population_type.load()
            with self.subTest(profile=population.profile):
                before = manage.Settings.load({**environment(), "LAB_AI_PROFILE": population.profile})
                after = manage.Settings.load({**environment(), "LAB_AI_PROFILE": population.profile,
                    "LAB_NPC_AGENTS_ENABLED": "1"})
                baseline = manage.config_overrides(before, population)
                enabled = manage.config_overrides(after, population)
                differences = {key: value for key, value in enabled.items() if value != baseline[key]}
                self.assertEqual(differences, {"AIWorld.NpcAgentEnabled": "1"})
                self.assertEqual(enabled["AIWorld.ScopeSpawnIds"], baseline["AIWorld.ScopeSpawnIds"])
                self.assertEqual(enabled["AIWorld.RemoteInferenceEnabled"], "0")
                self.assertEqual(enabled["AIWorld.DynamicTaskEnable"], "0")
                self.assertEqual(enabled["AIWorld.RecoveryAdviceEnabled"], "0")

    def test_unsupported_profiles_and_nonboolean_enablement_fail_before_render(self):
        for profile in ("disabled", "settlement-v8", "wildlife-v8", "supply-v8", "single-return", *hunt_population.PROFILES):
            with self.subTest(profile=profile), self.assertRaises(manage.SetupError):
                manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile, "LAB_NPC_AGENTS_ENABLED": "1"})
        for setting in ("true", "2", "-1", "1\nRealmID=1"):
            with self.subTest(setting=setting), self.assertRaises(manage.SetupError):
                manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumer_population.PROFILE,
                    "LAB_NPC_AGENTS_ENABLED": setting})
        settings = replace(manage.Settings.load(environment()), npc_agents_enabled=True)
        with self.assertRaises(manage.SetupError):
            manage.config_overrides(settings)

    def test_wildlife_pause_also_pauses_npc_ownership(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumer_population.PROFILE,
            "LAB_NPC_AGENTS_ENABLED": "1", "LAB_WILDLIFE_ENABLED": "0"})
        overrides = manage.config_overrides(settings)
        self.assertEqual(overrides["AIWorld.NpcAgentEnabled"], "0")
        self.assertEqual(overrides["AIWorld.Enable"], "0")

    def test_timing_and_admission_overrides_stay_inside_engine_bounds(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumer_population.PROFILE,
            "LAB_NPC_AGENTS_ENABLED": "1", "LAB_NPC_AGENTS_DECISION_INTERVAL_MS": "2000",
            "LAB_NPC_AGENTS_MAX_IN_FLIGHT": "4", "LAB_NPC_AGENTS_TIMEOUT_MS": "35000"})
        overrides = manage.config_overrides(settings)
        self.assertEqual(overrides["AIWorld.NpcAgentDecisionIntervalMs"], "2000")
        self.assertEqual(overrides["AIWorld.NpcAgentMaxInFlight"], "4")
        self.assertEqual(overrides["AIWorld.NpcAgentTimeoutMs"], "35000")
        for key, values in {
            "LAB_NPC_AGENTS_DECISION_INTERVAL_MS": ("1999", "-1", str(2**32), "2.5"),
            "LAB_NPC_AGENTS_MAX_IN_FLIGHT": ("0", "5", "-1", "2.5"),
            "LAB_NPC_AGENTS_TIMEOUT_MS": ("999", "35001", "-1", "15 seconds"),
        }.items():
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(manage.SetupError):
                    manage.Settings.load({**environment(), key: value})

    def test_both_templates_render_exactly_one_copy_of_each_npc_setting(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumer_population.PROFILE,
            "LAB_NPC_AGENTS_ENABLED": "1"})
        for template_path in ("deploy/worldserver.conf", "src/server/worldserver/worldserver.conf.dist"):
            with self.subTest(template=template_path):
                template = (ROOT / template_path).read_text(encoding="utf-8")
                rendered = manage.render_config(template, settings)
                for key, expected in NPC_DEFAULTS.items():
                    expected = "1" if key == "AIWorld.NpcAgentEnabled" else expected
                    self.assertEqual(rendered.count(f"\n{key} = "), 1)
                    self.assertIn(f"{key} = {expected}\n", rendered)
                self.assertIn("AIWorld.RemoteInferenceEnabled = 0\n", rendered)
                with self.assertRaises(manage.SetupError):
                    manage.render_config(template.replace("AIWorld.NpcAgentEnabled = 0", "# removed"), settings)

    def test_compose_exposes_independent_models_and_readonly_profiles(self):
        services = yaml.safe_load((ROOT / "compose.yml").read_text(encoding="utf-8"))["services"]
        world = services["worldserver"]
        self.assertIn("./data/npc_agents:/workspace/data/npc_agents:ro", world["volumes"])
        self.assertEqual(world["environment"]["LAB_NPC_AGENTS_ENABLED"], "${LAB_NPC_AGENTS_ENABLED:-0}")
        self.assertEqual(world["environment"]["LAB_NPC_AGENTS_TIMEOUT_MS"], "${LAB_NPC_AGENTS_TIMEOUT_MS:-15000}")
        model = services["ai-server"]["environment"]
        self.assertEqual(model["AI_NPC_MODEL_ENABLED"], "${AI_NPC_MODEL_ENABLED:-0}")
        for key in ("AI_NPC_MODEL_URL", "AI_NPC_MODEL_NAME", "AI_NPC_MODEL_API_KEY"):
            self.assertEqual(model[key], "${" + key + ":-}")
            self.assertNotIn(key, world["environment"])
        self.assertEqual(model["AI_NPC_MODEL_TIMEOUT_MS"], "${AI_NPC_MODEL_TIMEOUT_MS:-10000}")
        self.assertEqual(model["AI_TASK_MODEL_ENABLED"], "0")
        self.assertEqual(model["AI_RECOVERY_MODEL_ENABLED"], "0")

    def test_example_npc_settings_match_and_leave_models_disabled(self):
        example = (ROOT / ".env.example").read_bytes()
        compatibility = (ROOT / "deploy/lab/.env.example").read_bytes()
        # Compatibility profiles may have a different conservative default;
        # the new model/ownership switches must agree and remain disabled.
        def npc_values(raw):
            return {key: value for line in raw.decode().splitlines()
                    for key, separator, value in (line.partition("="),)
                    if separator and key.startswith(("LAB_NPC_", "AI_NPC_"))}
        self.assertEqual(npc_values(example), npc_values(compatibility))
        entries = dict(line.split("=", 1) for line in example.decode("utf-8").splitlines()
            if line and not line.startswith("#"))
        self.assertEqual(entries["LAB_NPC_AGENTS_ENABLED"], "0")
        self.assertEqual(entries["AI_NPC_MODEL_ENABLED"], "0")
        self.assertEqual(entries["AI_NPC_MODEL_API_KEY"], "")


if __name__ == "__main__":
    unittest.main()
