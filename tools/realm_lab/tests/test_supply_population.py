"""The opt-in supply cohort cannot redefine the reviewed authored world."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, settlement_population as settlement, single_return, supply_population as supply
from tools.realm_lab import telemetry_contract, wildlife_population as wildlife, world_setup
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_telemetry_contract import frame
from tools.realm_lab.tests.test_wildlife_population import fixture as wildlife_fixture, snapshot as wildlife_snapshot, write_json

ROOT = Path(__file__).resolve().parents[3]


def write_supply_metadata(metadata):
    document = single_return.load_json(settlement.METADATA / supply.MANIFEST)
    for field, filename in (("settlement_manifest_sha256", settlement.MANIFEST),
                            ("placement_proof_sha256", settlement.POSITION_PROOF),
                            ("wildlife_manifest_sha256", wildlife.MANIFEST)):
        document[field] = hashlib.sha256((metadata / filename).read_bytes()).hexdigest()
    write_json(metadata / supply.MANIFEST, document)
    return document["wildlife_manifest_sha256"]


def fixture(root):
    metadata, data, native_sha, proof_sha = wildlife_fixture(root)
    wildlife_sha = write_supply_metadata(metadata)
    return metadata, data, native_sha, proof_sha, wildlife_sha


def snapshot(population, control=1, *, legacy=True, hunters=None):
    rows = wildlife_snapshot(population.wildlife, control, legacy=legacy)
    selected = supply.HUNTER_IDS if hunters is None else set(hunters)
    for actor in population.hunters:
        if actor.spawn_id in selected:
            rows.extend([
                {"kind": "agent", "id": actor.spawn_id, "spawn": actor.spawn_id, "map": 725,
                 "type": 5, "control": control, "home_map": 725, "home_x": actor.x,
                 "home_y": actor.y, "home_z": actor.z, "home_o": actor.orientation,
                 "work_map": None, "faction": actor.world_faction},
                {"kind": "participation", "spawn": actor.spawn_id, "mode": 0},
                {"kind": "supply_work", "id": actor.spawn_id, "spawn": actor.spawn_id,
                 "work_map": None, "work_x": None, "work_y": None, "work_z": None, "work_o": None},
            ])
    return rows


class PublishedSupplyTests(unittest.TestCase):
    def test_three_explicit_combatant_hunters_preserve_native_homes_and_templates(self):
        population = supply.Population.load()
        self.assertEqual({actor.spawn_id for actor in population.hunters}, {901034, 901061, 901062})
        self.assertEqual(len(population.actors), 23)
        self.assertEqual(len(supply.NATIVE_IDS), 77)
        originals = {actor.spawn_id: actor for actor in population.settlement.actors}
        for actor in population.hunters:
            original = originals[actor.spawn_id]
            self.assertEqual((actor.entry, actor.label, actor.x, actor.y, actor.z, actor.orientation),
                             tuple(getattr(original, field) for field in ("entry", "label", "x", "y", "z", "orientation")))
            self.assertEqual(actor.agent_type, 5)
            self.assertEqual(actor.world_faction, 1 if actor.spawn_id == 901034 else 3)
        self.assertEqual(hashlib.sha256((settlement.METADATA / wildlife.MANIFEST).read_bytes()).hexdigest(),
                         supply.BASE_WILDLIFE_SHA256)

    def test_opt_in_flag_and_shared_pause_keep_exact_hundred_actor_scope(self):
        population = supply.Population.load()
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": supply.PROFILE})
        active = manage.config_overrides(settings, population)
        self.assertEqual(active["AIWorld.LivingSupplyEnabled"], "1")
        self.assertEqual(active["AIWorld.ScopeSpawnIds"], population.settlement.config()["AIWorld.ScopeSpawnIds"])
        for profile in ("disabled", settlement.PROFILE, wildlife.PROFILE):
            other = manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile})
            self.assertEqual(manage.config_overrides(other)["AIWorld.LivingSupplyEnabled"], "0")
        paused = manage.config_overrides(replace(settings, wildlife_enabled=False), population)
        for key in ("Enable", "LivingRolesEnabled", "LivingRoleExtensionsEnabled", "LivingNeedEvolutionEnabled", "LivingSupplyEnabled"):
            self.assertEqual(paused["AIWorld." + key], "0")
        self.assertEqual(paused["AIWorld.TelemetryNativeCreaturesEnabled"], "1")
        self.assertEqual(population.observer_environment(), population.settlement.observer_environment())
        rendered = manage.render_config((ROOT / "deploy/worldserver.conf").read_text(), settings, population)
        self.assertIn("AIWorld.LivingSupplyEnabled = 1\n", rendered)
        with self.assertRaises(manage.SetupError):
            manage.config_overrides(settings, population.wildlife)

    def test_repeated_schema_setup_permits_only_the_twenty_three_exact_bindings(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": supply.PROFILE})
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(settings)
        query = sql.call_args_list[-1].args[1]
        self.assertIn("agent_id NOT IN (901034,901061,901062,901072,", query)
        self.assertIn("spawn_id<>agent_id OR map_id<>725", query)
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "1"]):
            with self.assertRaises(manage.SetupError):
                world_setup.demote_retired_pilots(settings)


class SupplyPopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data, native_sha, proof_sha, wildlife_sha = fixture(Path(self.tmp.name))
        for module, name, value in ((wildlife, "BASE_MANIFEST_SHA256", native_sha),
                                    (wildlife, "BASE_PLACEMENT_PROOF_SHA256", proof_sha),
                                    (supply, "BASE_WILDLIFE_SHA256", wildlife_sha)):
            patcher = patch.object(module, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.population = supply.Population.load(self.metadata)

    def test_geometry_wildlife_pins_and_runtime_anchors_are_not_editable(self):
        self.assertEqual(supply.verify_data(self.data, self.metadata), self.population)
        original = single_return.load_json(self.metadata / supply.MANIFEST)
        mutations = (lambda d: d.update(profile=wildlife.PROFILE), lambda d: d.update(map_id=0),
                     lambda d: d["owned_spawn_ids"].pop(), lambda d: d["native_spawn_ids"].pop(),
                     lambda d: d.update(runtime_food_state_only=False),
                     lambda d: d["depots"][0]["position"].update(x=0),
                     lambda d: d["hunting_grounds"][0].update(radius_yards=1000),
                     lambda d: d["hunters"][0].update(agent_type=6),
                     lambda d: d["hunters"][0].update(world_faction_id=3),
                     lambda d: d["hunters"][0]["home"].update(x=0),
                     lambda d: d["hunters"][0].update(entry=1186),
                     lambda d: d["hunters"][0].update(depot_id=2))
        for mutate in mutations:
            candidate = copy.deepcopy(original)
            mutate(candidate)
            write_json(self.metadata / supply.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(supply.ProfileError):
                supply.Population.load(self.metadata)
        write_json(self.metadata / supply.MANIFEST, original)
        path = self.data / "mmaps/7253130.mmtile"
        path.write_bytes(b"changed terrain")
        with self.assertRaises(supply.ProfileError):
            supply.verify_data(self.data, self.metadata)

    def test_active_observe_bootstrap_and_existing_wildlife_transition_are_explicit(self):
        for control in (0, 1):
            for legacy in (False, True):
                rows = snapshot(self.population, control, legacy=legacy)
                original = copy.deepcopy(rows)
                supply.validate_snapshot(rows, self.population, control=control)
                self.assertEqual(rows, original)
        rows = snapshot(self.population, hunters=())
        supply.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        with self.assertRaises(supply.ProfileError):
            supply.validate_snapshot(rows, self.population)
        rows = snapshot(self.population, hunters={901034})
        next(r for r in rows if r["kind"] == "agent" and r["id"] == 901034)["control"] = 0
        supply.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        with self.assertRaises(supply.ProfileError):
            supply.validate_snapshot(rows, self.population, control=None)

    def test_hunter_binding_fields_and_wholly_absent_work_are_guarded_before_filtering(self):
        changes = {"id": 901035, "spawn": 901035, "map": 0, "type": 3, "control": 2,
                   "home_map": 0, "home_x": 0, "home_y": 0, "home_z": 0, "home_o": 0,
                   "work_map": 725, "faction": 3}
        for field, value in changes.items():
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == "agent" and r["id"] == 901034)[field] = value
            with self.subTest(field=field), self.assertRaises(supply.ProfileError):
                supply.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for field in ("work_map", "work_x", "work_y", "work_z", "work_o"):
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == "supply_work")[field] = 0
            with self.subTest(field=field), self.assertRaises(supply.ProfileError):
                supply.validate_snapshot(rows, self.population)
        for kind in ("agent", "participation", "supply_work"):
            rows = snapshot(self.population)
            duplicate = next(r for r in rows if r["kind"] == kind and r.get("id", r.get("spawn")) == 901034)
            with self.subTest(kind=kind), self.assertRaises(supply.ProfileError):
                supply.validate_snapshot(rows + [copy.deepcopy(duplicate)], self.population)

    def test_native_wildlife_legacy_groups_and_unknown_state_keep_the_original_guards(self):
        for kind, identity, field, value in (("spawn", 901035, "x", 0),
                                            ("agent", 901072, "type", 6),
                                            ("agent", 900725, "control", 1)):
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == kind and r.get("id", r.get("guid")) == identity)[field] = value
            with self.subTest(kind=kind), self.assertRaises(supply.ProfileError):
                supply.validate_snapshot(rows, self.population)
        for extra in ({"kind": "membership", "group": 1, "member": 901034},
                      {"kind": "supply_work", "id": 901035, "spawn": 901035,
                       "work_map": None, "work_x": None, "work_y": None, "work_z": None, "work_o": None},
                      {"kind": "agent", "id": 901034}, {"kind": "unknown"}):
            with self.subTest(extra=extra), self.assertRaises(supply.ProfileError):
                supply.validate_snapshot(snapshot(self.population) + [extra], self.population)

    def test_additive_sql_does_not_change_shared_templates_classification_or_economy(self):
        bootstrap = supply.bootstrap_sql(self.population)
        activate = supply.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 3)
        self.assertNotIn("ai_agent_type_entry_defaults", bootstrap)
        self.assertEqual(activate.count("UPDATE characters.ai_agents"), 1)
        self.assertIn("COUNT(*)=20", activate)
        self.assertIn("COUNT(*)=3", activate)
        self.assertIn("ct.entry=11328", activate)
        self.assertIn("ct.entry=478", activate)
        self.assertIn("@supply_population_ready=1", activate)
        self.assertIn("agent_id IN (" + self.population.spawn_ids + ")", activate)
        for sql in (bootstrap, activate, supply.deactivation_sql(self.population)):
            for forbidden in ("auth.", "DELETE", "ALTER", "UPDATE world.", "SET home_", "food=", "money=", "resource="):
                self.assertNotIn(forbidden, sql)

    def test_offline_bootstrap_preserves_wildlife_before_adding_and_activating_three_hunters(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": supply.PROFILE})
        before, complete, active = (snapshot(self.population, hunters=()), snapshot(self.population, 0), snapshot(self.population))
        reads = iter((before, complete, active))
        statements = []
        def sql(settings, query):
            statements.append(query)
            return "\n".join(json.dumps(row) for row in next(reads)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        with patch.object(supply, "verify_data", return_value=self.population), \
             patch.object(settlement, "ensure_world_offline") as guard, \
             patch.object(manage, "lab_mysql", side_effect=sql):
            manage.bootstrap_supply(self.data, settings)
        self.assertGreaterEqual(guard.call_count, 3)
        self.assertEqual(sum("@supply_population_ready" in q for q in statements), 1)
        self.assertFalse(any("DELETE" in q for q in statements))


class SupplyTelemetryTests(unittest.TestCase):
    def test_twenty_three_roles_and_pause_are_distinct_from_wildlife_only(self):
        candidate = frame(True)
        for actor in candidate["agents"]:
            if actor["spawn_id"] in supply.HUNTER_IDS:
                actor.update(native_creature=False, control_mode="AI_WORLD_CONTROLLED", needs={"hunger": 0.2}, economy={},
                             living_role={"enabled": True, "extensions_enabled": True, "role": "HUNTER", "lifetime_ms": 1})
                depot = 1 if actor["spawn_id"] == 901034 else 2
                actor["supply"] = {"enabled": True, "role": "HUNTER", "depot_id": depot, "cargo": 0, "capacity": 6,
                                   "depot": {"id": depot, "position": {"map_id": 725}, "stock": 0, "capacity": 24},
                                   "accounting": {"conserved": True}}
        result = telemetry_contract.validate_snapshot(candidate, supply.PROFILE)
        self.assertEqual((result["managed"], result["native"], result["live_roles"]), (23, 77, 23))
        for profile in (settlement.PROFILE, wildlife.PROFILE):
            with self.assertRaises(settlement.ProfileError):
                telemetry_contract.validate_snapshot(candidate, profile)
        self.assertEqual(telemetry_contract.validate_snapshot(frame(), supply.PROFILE, False)["managed"], 0)
        with self.assertRaises(settlement.ProfileError):
            telemetry_contract.validate_snapshot(frame(True), supply.PROFILE)
        hunter = next(a for a in candidate["agents"] if a["spawn_id"] == 901034)
        hunter["supply"]["accounting"]["conserved"] = False
        with self.assertRaises(settlement.ProfileError):
            telemetry_contract.validate_snapshot(candidate, supply.PROFILE)


if __name__ == "__main__":
    unittest.main()
