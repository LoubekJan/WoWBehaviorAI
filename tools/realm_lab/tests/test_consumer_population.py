"""Ordinary consumer ownership preserves the existing supply and native world."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import consumer_population as consumers, manage, settlement_population as settlement
from tools.realm_lab import single_return, supply_population as supply, telemetry_contract, wildlife_population as wildlife, world_setup
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_telemetry_contract import frame as wildlife_frame
from tools.realm_lab.tests.test_supply_population import fixture as supply_fixture, snapshot as supply_snapshot
from tools.realm_lab.tests.test_wildlife_population import write_json

ROOT = Path(__file__).resolve().parents[3]


def write_consumer_metadata(metadata):
    document = single_return.load_json(settlement.METADATA / consumers.MANIFEST)
    for field, filename in (("settlement_manifest_sha256", settlement.MANIFEST),
                            ("placement_proof_sha256", settlement.POSITION_PROOF),
                            ("wildlife_manifest_sha256", wildlife.MANIFEST),
                            ("supply_manifest_sha256", supply.MANIFEST)):
        document[field] = hashlib.sha256((metadata / filename).read_bytes()).hexdigest()
    write_json(metadata / consumers.MANIFEST, document)
    return document["supply_manifest_sha256"]


def fixture(root):
    metadata, data, native_sha, proof_sha, wildlife_sha = supply_fixture(root)
    supply_sha = write_consumer_metadata(metadata)
    return metadata, data, native_sha, proof_sha, wildlife_sha, supply_sha


def snapshot(population, control=1, *, legacy=True, selected=None):
    rows = supply_snapshot(population.supply, control, legacy=legacy)
    selected = consumers.CONSUMER_IDS if selected is None else set(selected)
    for actor in population.consumers:
        if actor.spawn_id in selected:
            rows.extend([
                {"kind": "agent", "id": actor.spawn_id, "spawn": actor.spawn_id, "map": 725,
                 "type": actor.agent_type, "control": control, "home_map": 725,
                 "home_x": actor.x, "home_y": actor.y, "home_z": actor.z, "home_o": actor.orientation,
                 "work_map": None, "faction": actor.world_faction},
                {"kind": "participation", "spawn": actor.spawn_id, "mode": 0},
                {"kind": "consumer_work", "id": actor.spawn_id, "spawn": actor.spawn_id,
                 "work_map": None, "work_x": None, "work_y": None, "work_z": None, "work_o": None},
            ])
    return rows


def frame():
    candidate = wildlife_frame(True)
    for actor in candidate["agents"]:
        sid = actor["spawn_id"]
        if sid not in supply.HUNTER_IDS | consumers.CONSUMER_IDS:
            continue
        consumer = sid in consumers.CONSUMER_IDS
        depot = consumers.CONSUMER_DEPOTS[sid] if consumer else 1 if sid == 901034 else 2
        actor.update(native_creature=False, control_mode="AI_WORLD_CONTROLLED", needs={"hunger": 0.2}, economy={},
                     type="CIVILIAN" if consumer and consumers.CONSUMER_TYPES[sid] == 0 else "COMBATANT",
                     world_faction=consumers.CONSUMER_FACTIONS[sid] if consumer else 1 if sid == 901034 else 3,
                     living_role={"enabled": True, "extensions_enabled": True,
                                  "role": consumers.CONSUMER_ROLES[sid] if consumer else "HUNTER", "lifetime_ms": 1})
        actor["supply"] = {"enabled": True, "role": "CONSUMER" if consumer else "HUNTER", "depot_id": depot,
                           "cargo": 0, "capacity": 0 if consumer else 6,
                           "depot": {"id": depot, "position": {"map_id": 725}, "stock": 0, "capacity": 30},
                           "accounting": {"conserved": True}}
    return candidate


class PublishedConsumerTests(unittest.TestCase):
    def test_preserves_ordinary_types_factions_homes_and_original_twenty_three(self):
        population = consumers.Population.load()
        self.assertEqual(len(population.actors), 26)
        self.assertEqual(len(consumers.NATIVE_IDS), 74)
        self.assertEqual(population.supply, supply.Population.load())
        original = {actor.spawn_id: actor for actor in population.settlement.actors}
        for actor in population.consumers:
            before = original[actor.spawn_id]
            self.assertEqual((actor.entry, actor.label, actor.role, actor.x, actor.y, actor.z, actor.orientation),
                             tuple(getattr(before, f) for f in ("entry", "label", "role", "x", "y", "z", "orientation")))
            self.assertEqual(actor.agent_type, 5 if actor.spawn_id == 901063 else 0)
            self.assertEqual(actor.world_faction, 3 if actor.spawn_id == 901063 else 1)
        self.assertEqual(hashlib.sha256((settlement.METADATA / supply.MANIFEST).read_bytes()).hexdigest(),
                         consumers.BASE_SUPPLY_SHA256)

    def test_new_capability_is_opt_in_and_pause_disables_all_twenty_six(self):
        population = consumers.Population.load()
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumers.PROFILE})
        active = manage.config_overrides(settings, population)
        for key in ("AIWorld.LivingConsumersEnabled", "AIWorld.LivingSupplyEnabled", "AIWorld.LivingRolesEnabled"):
            self.assertEqual(active[key], "1")
        self.assertEqual(active["AIWorld.ScopeSpawnIds"], population.settlement.config()["AIWorld.ScopeSpawnIds"])
        for profile in ("disabled", settlement.PROFILE, wildlife.PROFILE, supply.PROFILE):
            self.assertEqual(manage.config_overrides(manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile}))[
                "AIWorld.LivingConsumersEnabled"], "0")
        paused = manage.config_overrides(replace(settings, wildlife_enabled=False), population)
        for key in ("Enable", "LivingRolesEnabled", "LivingRoleExtensionsEnabled", "LivingNeedEvolutionEnabled",
                    "LivingSupplyEnabled", "LivingConsumersEnabled"):
            self.assertEqual(paused["AIWorld." + key], "0")
        self.assertEqual(paused["AIWorld.TelemetryNativeCreaturesEnabled"], "1")
        rendered = manage.render_config((ROOT / "deploy/worldserver.conf").read_text(), settings, population)
        self.assertIn("AIWorld.LivingConsumersEnabled = 1\n", rendered)
        with self.assertRaises(manage.SetupError):
            manage.config_overrides(settings, population.supply)

    def test_repeated_world_setup_allows_only_the_twenty_six_reviewed_records(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumers.PROFILE})
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(settings)
        query = sql.call_args_list[-1].args[1]
        self.assertIn("agent_id NOT IN (" + consumers.Population.load().spawn_ids + ")", query)
        self.assertIn("spawn_id<>agent_id OR map_id<>725", query)


class ConsumerPopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data, native_sha, proof_sha, wildlife_sha, supply_sha = fixture(Path(self.tmp.name))
        for module, name, value in ((wildlife, "BASE_MANIFEST_SHA256", native_sha),
                                    (wildlife, "BASE_PLACEMENT_PROOF_SHA256", proof_sha),
                                    (supply, "BASE_WILDLIFE_SHA256", wildlife_sha),
                                    (consumers, "BASE_SUPPLY_SHA256", supply_sha)):
            patcher = patch.object(module, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.population = consumers.Population.load(self.metadata)

    def test_manifest_and_geometry_cannot_retype_move_or_expand_consumers(self):
        self.assertEqual(consumers.verify_data(self.data, self.metadata), self.population)
        original = single_return.load_json(self.metadata / consumers.MANIFEST)
        mutations = (lambda d: d.update(profile=supply.PROFILE), lambda d: d.update(map_id=0),
                     lambda d: d["owned_spawn_ids"].pop(), lambda d: d["native_spawn_ids"].pop(),
                     lambda d: d.update(runtime_food_state_only=False), lambda d: d["consumers"].pop(),
                     lambda d: d["consumers"][0].update(agent_type=5),
                     lambda d: d["consumers"][0].update(world_faction_id=3),
                     lambda d: d["consumers"][0].update(role="hunter"),
                     lambda d: d["consumers"][0].update(living_role="HUNTER"),
                     lambda d: d["consumers"][0].update(capability="hunter"),
                     lambda d: d["consumers"][0].update(depot_id=2),
                     lambda d: d["consumers"][0]["home"].update(x=0))
        for mutate in mutations:
            candidate = copy.deepcopy(original)
            mutate(candidate)
            write_json(self.metadata / consumers.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(consumers.ProfileError):
                consumers.Population.load(self.metadata)
        write_json(self.metadata / consumers.MANIFEST, original)
        path = self.data / "mmaps/7253130.mmtile"
        path.write_bytes(b"different geometry")
        with self.assertRaises(consumers.ProfileError):
            consumers.verify_data(self.data, self.metadata)

    def test_unchanged_supply_can_transition_only_through_explicit_bootstrap(self):
        for control in (0, 1):
            for legacy in (False, True):
                rows = snapshot(self.population, control, legacy=legacy)
                before = copy.deepcopy(rows)
                consumers.validate_snapshot(rows, self.population, control=control)
                self.assertEqual(rows, before)
        before = snapshot(self.population, selected=())
        consumers.validate_snapshot(before, self.population, control=None, bootstrap=True)
        with self.assertRaises(consumers.ProfileError):
            consumers.validate_snapshot(before, self.population)
        partial = snapshot(self.population, selected={901035})
        next(r for r in partial if r["kind"] == "agent" and r["id"] == 901035)["control"] = 0
        consumers.validate_snapshot(partial, self.population, control=None, bootstrap=True)
        with self.assertRaises(consumers.ProfileError):
            consumers.validate_snapshot(partial, self.population, control=None)

    def test_all_consumer_binding_fields_are_validated_before_filtering(self):
        changes = {"id": 901037, "spawn": 901037, "map": 0, "type": 5, "control": 2, "home_map": 0,
                   "home_x": 0, "home_y": 0, "home_z": 0, "home_o": 0, "work_map": 725, "faction": 3}
        for field, value in changes.items():
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == "agent" and r["id"] == 901035)[field] = value
            with self.subTest(field=field), self.assertRaises(consumers.ProfileError):
                consumers.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for field in ("work_map", "work_x", "work_y", "work_z", "work_o"):
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == "consumer_work")[field] = 0
            with self.subTest(field=field), self.assertRaises(consumers.ProfileError):
                consumers.validate_snapshot(rows, self.population)

    def test_existing_supply_wildlife_native_and_legacy_guards_are_not_exempted(self):
        for kind, identity, field, value in (("agent", 901034, "type", 0),
                                            ("agent", 901072, "type", 6),
                                            ("agent", 900725, "control", 1),
                                            ("spawn", 901037, "x", 0)):
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == kind and r.get("id", r.get("guid")) == identity)[field] = value
            with self.subTest(identity=identity), self.assertRaises(consumers.ProfileError):
                consumers.validate_snapshot(rows, self.population)
        for extra in ({"kind": "membership", "group": 1, "member": 901035},
                      {"kind": "consumer_work", "id": 901037, "spawn": 901037,
                       "work_map": None, "work_x": None, "work_y": None, "work_z": None, "work_o": None},
                      {"kind": "agent", "id": 901035}, {"kind": "unknown"}):
            with self.subTest(extra=extra), self.assertRaises(consumers.ProfileError):
                consumers.validate_snapshot(snapshot(self.population) + [extra], self.population)
        for kind in ("agent", "participation", "consumer_work"):
            rows = snapshot(self.population)
            duplicate = next(r for r in rows if r["kind"] == kind and r.get("id", r.get("spawn")) == 901035)
            with self.subTest(kind=kind), self.assertRaises(consumers.ProfileError):
                consumers.validate_snapshot(rows + [copy.deepcopy(duplicate)], self.population)

    def test_sql_is_additive_and_does_not_retype_or_reward_existing_agents(self):
        bootstrap = consumers.bootstrap_sql(self.population)
        activate = consumers.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 3)
        self.assertNotIn("ai_agent_type_entry_defaults", bootstrap)
        self.assertEqual(activate.count("UPDATE characters.ai_agents"), 1)
        self.assertIn("a.agent_type=0", activate)
        self.assertIn("ct.entry=97", activate)
        self.assertIn("ct.entry=11328", activate)
        self.assertIn("agent_id IN (" + self.population.spawn_ids + ")", activate)
        self.assertIn("@consumer_population_ready=1", activate)
        for sql in (bootstrap, activate, consumers.deactivation_sql(self.population)):
            for forbidden in ("auth.", "DELETE", "ALTER", "UPDATE world.", "SET home_", "SET agent_type", "food=", "money=", "resource="):
                self.assertNotIn(forbidden, sql)

    def test_offline_setup_adds_three_consumers_without_resetting_existing_supply(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": consumers.PROFILE})
        before = snapshot(self.population, selected=())
        complete, active = snapshot(self.population, 0), snapshot(self.population)
        reads = iter((before, complete, active))
        statements = []
        def sql(settings, query):
            statements.append(query)
            return "\n".join(json.dumps(row) for row in next(reads)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        with patch.object(consumers, "verify_data", return_value=self.population), \
             patch.object(settlement, "ensure_world_offline") as guard, \
             patch.object(manage, "lab_mysql", side_effect=sql):
            manage.bootstrap_consumers(self.data, settings)
        self.assertGreaterEqual(guard.call_count, 3)
        self.assertEqual(sum("@consumer_population_ready" in q for q in statements), 1)
        self.assertFalse(any("DELETE" in q or "SET agent_type" in q for q in statements))


class ConsumerTelemetryTests(unittest.TestCase):
    def test_twenty_six_ordinary_roles_and_shared_pause_are_strict(self):
        candidate = frame()
        result = telemetry_contract.validate_snapshot(candidate, consumers.PROFILE)
        self.assertEqual((result["managed"], result["native"], result["live_roles"]), (26, 74, 26))
        self.assertEqual(telemetry_contract.validate_snapshot(wildlife_frame(), consumers.PROFILE, False)["managed"], 0)
        for profile in (settlement.PROFILE, wildlife.PROFILE, supply.PROFILE):
            with self.assertRaises(settlement.ProfileError):
                telemetry_contract.validate_snapshot(candidate, profile)

    def test_consumer_cannot_become_hunter_carry_food_change_faction_or_hide_accounting(self):
        mutations = (lambda a: a.update(type="COMBATANT"), lambda a: a.update(world_faction=3),
                     lambda a: a["living_role"].update(role="HUNTER"),
                     lambda a: a["supply"].update(role="HUNTER"), lambda a: a["supply"].update(capacity=6),
                     lambda a: a["supply"].update(cargo=1), lambda a: a["supply"].update(depot_id=2),
                     lambda a: a["supply"]["accounting"].update(conserved=False))
        for mutate in mutations:
            candidate = frame()
            mutate(next(a for a in candidate["agents"] if a["spawn_id"] == 901035))
            with self.subTest(mutate=mutate), self.assertRaises(settlement.ProfileError):
                telemetry_contract.validate_snapshot(candidate, consumers.PROFILE)


if __name__ == "__main__":
    unittest.main()
