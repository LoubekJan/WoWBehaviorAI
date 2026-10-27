"""Raid reservations retain the complete reviewed consumer/native baseline."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import raid_population as raids, consumer_population as consumers, manage
from tools.realm_lab import settlement_population as settlement, single_return, supply_population as supply
from tools.realm_lab import telemetry_contract, wildlife_population as wildlife, world_setup
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_consumer_population import fixture as consumer_fixture, snapshot as consumer_snapshot, frame as consumer_frame
from tools.realm_lab.tests.test_telemetry_contract import frame as wildlife_frame
from tools.realm_lab.tests.test_wildlife_population import write_json

ROOT = Path(__file__).resolve().parents[3]


def write_raid_metadata(metadata):
    document = single_return.load_json(raids.METADATA / raids.MANIFEST)
    for field, filename in (("consumer_manifest_sha256", consumers.MANIFEST),
                            ("supply_manifest_sha256", supply.MANIFEST),
                            ("wildlife_manifest_sha256", wildlife.MANIFEST),
                            ("settlement_manifest_sha256", settlement.MANIFEST),
                            ("placement_proof_sha256", settlement.POSITION_PROOF)):
        document[field] = hashlib.sha256((metadata / filename).read_bytes()).hexdigest()
    write_json(metadata / raids.MANIFEST, document)
    return document["consumer_manifest_sha256"]


def snapshot(population, control=1, *, selected=None):
    rows = consumer_snapshot(population.consumers, control)
    selected = raids.RECRUIT_IDS if selected is None else set(selected)
    for actor in population.recruits:
        if actor.spawn_id in selected:
            rows.extend([
                {"kind": "agent", "id": actor.spawn_id, "spawn": actor.spawn_id, "map": 725,
                 "type": actor.agent_type, "control": control, "home_map": 725,
                 "home_x": actor.x, "home_y": actor.y, "home_z": actor.z, "home_o": actor.orientation,
                 "work_map": None, "faction": actor.world_faction},
                {"kind": "participation", "spawn": actor.spawn_id, "mode": 0},
                {"kind": "raid_work", "id": actor.spawn_id, "spawn": actor.spawn_id,
                 "work_map": None, "work_x": None, "work_y": None, "work_z": None, "work_o": None},
            ])
    return rows


def frame():
    candidate = consumer_frame()
    for actor in candidate["agents"]:
        sid = actor["spawn_id"]
        if sid not in raids.RECRUIT_IDS:
            continue
        raider = sid in raids.RAIDER_IDS
        depot_id = 3 if raider else 1
        depot = {"id": depot_id, "position": copy.deepcopy((raids.DEPOT if raider else supply.DEPOTS[0])["position"]),
                 "stock": 0, "capacity": 30}
        actor.update(native_creature=False, control_mode="AI_WORLD_CONTROLLED", needs={"hunger": 0.2}, economy={},
                     type="COMBATANT" if raider else "GUARD", world_faction=2 if raider else 1,
                     living_role={"enabled": True, "extensions_enabled": True, "role": raids.RECRUIT_ROLES[sid], "lifetime_ms": 1})
        actor["supply"] = {"enabled": True, "role": "RAIDER" if raider else "CONSUMER", "depot_id": depot_id,
                           "cargo": 0, "capacity": 2 if raider else 0, "depot": depot, "accounting": {"conserved": True}}
        actor["raid"] = {"enabled": True, "role": raids.RECRUIT_ROLES[sid], "source_depot_id": 1, "camp_depot_id": 3,
                         "source_depot": {"id": 1, "position": copy.deepcopy(supply.DEPOTS[0]["position"]), "stock": 0, "capacity": 30},
                         "camp_depot": {"id": 3, "position": copy.deepcopy(raids.DEPOT["position"]), "stock": 0, "capacity": 30}}
    return candidate


class PublishedRaidTests(unittest.TestCase):
    def test_original_twenty_six_and_native_homes_types_are_immutable(self):
        population = raids.Population.load()
        self.assertEqual(population.consumers, consumers.Population.load())
        self.assertEqual((len(population.actors), len(raids.NATIVE_IDS)), (28, 72))
        before = {actor.spawn_id: actor for actor in population.settlement.actors}
        for actor in population.recruits:
            self.assertEqual(tuple(getattr(actor, field) for field in ("entry", "label", "role", "x", "y", "z", "orientation")),
                             tuple(getattr(before[actor.spawn_id], field) for field in ("entry", "label", "role", "x", "y", "z", "orientation")))
        self.assertEqual([(a.spawn_id, a.agent_type, a.world_faction) for a in population.recruits], [(901039, 1, 1), (901055, 5, 2)])
        self.assertEqual(len(raids.TRANSIT_WAYPOINTS), 8)
        self.assertEqual([(point["x"], point["y"], point["z"]) for point in raids.TRANSIT_WAYPOINTS[1:4]],
                         [(-160.0, 860.0, 0.15),
                          (-201.60006713867188, 861.0665283203125, -2.2201099395751953),
                          (-235.0, 790.0, -0.62)])

    def test_profile_opt_in_shared_pause_and_setup_roster(self):
        population = raids.Population.load()
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": raids.PROFILE})
        active = manage.config_overrides(settings, population)
        for flag in ("LivingRaidsEnabled", "LivingConsumersEnabled", "LivingSupplyEnabled"):
            self.assertEqual(active["AIWorld." + flag], "1")
            self.assertEqual(manage.config_overrides(replace(settings, wildlife_enabled=False), population)["AIWorld." + flag], "0")
        for profile in ("disabled", settlement.PROFILE, wildlife.PROFILE, supply.PROFILE, consumers.PROFILE):
            self.assertEqual(manage.config_overrides(manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile}))["AIWorld.LivingRaidsEnabled"], "0")
        with self.assertRaises(manage.SetupError):
            manage.config_overrides(settings, population.consumers)
        with patch.object(settlement, "ensure_world_offline"), patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(settings)
        self.assertIn("agent_id NOT IN (" + population.spawn_ids + ")", sql.call_args_list[-1].args[1])
        rendered = manage.render_config((ROOT / "deploy/worldserver.conf").read_text(), settings, population)
        self.assertIn("AIWorld.LivingRaidsEnabled = 1\n", rendered)


class RaidPopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data, native_sha, proof_sha, wildlife_sha, supply_sha = consumer_fixture(Path(self.tmp.name))
        consumer_sha = write_raid_metadata(self.metadata)
        for module, field, value in ((wildlife, "BASE_MANIFEST_SHA256", native_sha),
                                     (wildlife, "BASE_PLACEMENT_PROOF_SHA256", proof_sha),
                                     (supply, "BASE_WILDLIFE_SHA256", wildlife_sha),
                                     (consumers, "BASE_SUPPLY_SHA256", supply_sha),
                                     (raids, "BASE_CONSUMERS_SHA256", consumer_sha)):
            patcher = patch.object(module, field, value)
            patcher.start(); self.addCleanup(patcher.stop)
        self.population = raids.Population.load(self.metadata)

    def test_manifest_cannot_expand_retype_reposition_or_change_camp(self):
        self.assertEqual(raids.verify_data(self.data, self.metadata), self.population)
        original = single_return.load_json(self.metadata / raids.MANIFEST)
        mutations = (lambda d: d.update(profile=consumers.PROFILE), lambda d: d.update(map_id=0),
                     lambda d: d["owned_spawn_ids"].pop(), lambda d: d["native_spawn_ids"].pop(),
                     lambda d: d.update(runtime_food_state_only=False), lambda d: d["recruits"].pop(),
                     lambda d: d["recruits"][0].update(agent_type=5), lambda d: d["recruits"][0].update(agent_type=True),
                     lambda d: d["recruits"][0].update(world_faction_id=2), lambda d: d["recruits"][0].update(living_role="HUNTER"),
                     lambda d: d["recruits"][0].update(capability="hunter"), lambda d: d["recruits"][0].update(cargo_capacity=2),
                     lambda d: d["recruits"][0]["home"].update(x=0), lambda d: d["camp_depot"]["position"].update(x=0),
                     lambda d: d["transit_waypoints"][0].update(x=0), lambda d: d["guard_post"].update(x=0),
                     lambda d: d.update(raider_max_home_distance_yards=901))
        for mutate in mutations:
            candidate = copy.deepcopy(original); mutate(candidate)
            write_json(self.metadata / raids.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(raids.ProfileError):
                raids.Population.load(self.metadata)
        write_json(self.metadata / raids.MANIFEST, original)
        (self.data / "mmaps/7253130.mmtile").write_bytes(b"changed geometry")
        with self.assertRaises(raids.ProfileError): raids.verify_data(self.data, self.metadata)

    def test_bootstrap_is_explicit_and_all_existing_guards_remain(self):
        for control in (0, 1): raids.validate_snapshot(snapshot(self.population, control), self.population, control=control)
        rows = snapshot(self.population, selected=())
        raids.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        with self.assertRaises(raids.ProfileError): raids.validate_snapshot(rows, self.population)
        for kind, identity, field, value in (("agent", 901035, "type", 5), ("agent", 901034, "type", 0),
                                            ("agent", 901072, "type", 5), ("agent", 900725, "control", 1),
                                            ("spawn", 901037, "x", 0)):
            rows = snapshot(self.population)
            next(r for r in rows if r["kind"] == kind and r.get("id", r.get("guid")) == identity)[field] = value
            with self.subTest(identity=identity), self.assertRaises(raids.ProfileError): raids.validate_snapshot(rows, self.population)

    def test_recruit_rows_are_fully_checked_before_filtering(self):
        for sid in raids.RECRUIT_IDS:
            for field, value in {"id": 901037, "spawn": 901037, "map": 0, "type": 0, "control": 2, "home_map": 0,
                                  "home_x": 0, "home_y": 0, "home_z": 0, "home_o": 0, "work_map": 725, "faction": 3}.items():
                rows = snapshot(self.population)
                next(r for r in rows if r["kind"] == "agent" and r["id"] == sid)[field] = value
                with self.subTest(sid=sid, field=field), self.assertRaises(raids.ProfileError):
                    raids.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for field in ("work_map", "work_x", "work_y", "work_z", "work_o"):
            rows = snapshot(self.population); next(r for r in rows if r["kind"] == "raid_work")[field] = 0
            with self.assertRaises(raids.ProfileError): raids.validate_snapshot(rows, self.population)
        for kind in ("agent", "participation", "raid_work"):
            rows = snapshot(self.population)
            row = next(r for r in rows if r["kind"] == kind and r.get("id", r.get("spawn")) == 901055)
            with self.assertRaises(raids.ProfileError): raids.validate_snapshot(rows + [copy.deepcopy(row)], self.population)
        with self.assertRaises(raids.ProfileError):
            raids.validate_snapshot(snapshot(self.population) + [{"kind": "membership", "group": 1, "member": 901055}], self.population)

    def test_sql_adds_only_two_reservations_and_setup_preserves_existing_twenty_six(self):
        bootstrap, activation = raids.bootstrap_sql(self.population), raids.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 2)
        self.assertEqual(activation.count("UPDATE characters.ai_agents"), 1)
        for text in ("a.agent_type=1", "a.agent_type=5", "ct.entry=116", "ct.entry=1423", "@raid_population_ready=1"):
            self.assertIn(text, activation)
        for sql in (bootstrap, activation, raids.deactivation_sql(self.population)):
            for forbidden in ("auth.", "DELETE", "ALTER", "UPDATE world.", "SET home_", "SET agent_type", "food=", "money=", "resource=", "ai_agent_type_entry_defaults ("):
                self.assertNotIn(forbidden, sql)
        reads = iter((snapshot(self.population, selected=()), snapshot(self.population, 0), snapshot(self.population)))
        def sql(settings, query):
            return "\n".join(json.dumps(row) for row in next(reads)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": raids.PROFILE})
        with patch.object(raids, "verify_data", return_value=self.population), patch.object(settlement, "ensure_world_offline") as guard, patch.object(manage, "lab_mysql", side_effect=sql):
            manage.bootstrap_raids(self.data, settings)
        self.assertGreaterEqual(guard.call_count, 3)


class RaidTelemetryTests(unittest.TestCase):
    def test_twenty_eight_strict_roles_and_pause(self):
        result = telemetry_contract.validate_snapshot(frame(), raids.PROFILE)
        self.assertEqual((result["managed"], result["native"], result["live_roles"]), (28, 72, 28))
        self.assertEqual(telemetry_contract.validate_snapshot(wildlife_frame(), raids.PROFILE, False)["managed"], 0)
        for profile in (settlement.PROFILE, wildlife.PROFILE, supply.PROFILE, consumers.PROFILE):
            with self.assertRaises(raids.ProfileError): telemetry_contract.validate_snapshot(frame(), profile)

    def test_raid_cannot_acquire_hunting_unbounded_cargo_or_changed_depots(self):
        for sid, section, key, value in ((901055, "supply", "capacity", 3), (901055, "supply", "cargo", 3),
                                        (901055, "supply", "role", "HUNTER"), (901055, "supply", "hunting_ground", {}),
                                        (901039, "supply", "cargo", 1), (901039, "raid", "role", "RAIDER"),
                                        (901055, "raid", "camp_depot_id", 1), (901055, "raid", "enabled", False)):
            state = frame(); actor = next(a for a in state["agents"] if a["spawn_id"] == sid)
            actor[section][key] = value
            with self.subTest(sid=sid, key=key), self.assertRaises(raids.ProfileError):
                telemetry_contract.validate_snapshot(state, raids.PROFILE)
        state = frame(); next(a for a in state["agents"] if a["spawn_id"] == 901055)["raid"]["camp_depot"]["position"]["x"] = 0
        with self.assertRaises(raids.ProfileError): telemetry_contract.validate_snapshot(state, raids.PROFILE)


if __name__ == "__main__": unittest.main()
