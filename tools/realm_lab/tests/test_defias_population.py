"""The whole Defias camp is an explicit overlay over the immutable field world."""
import copy
from dataclasses import replace
import hashlib
import json
import unittest
from unittest.mock import patch

from tools.realm_lab import defias_population as defias, field_population as fields, manage
from tools.realm_lab import raid_population as raids, settlement_population as settlement, single_return, telemetry_contract, world_setup
from tools.realm_lab.tests import test_field_population as field_tests
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_wildlife_population import write_json


def write_defias_metadata(metadata, population):
    sha = hashlib.sha256((metadata / fields.MANIFEST).read_bytes()).hexdigest()
    with patch.object(defias, "BASE_FIELDS_SHA256", sha):
        write_json(metadata / defias.MANIFEST, defias.manifest_document(population))
    return sha


def snapshot(population, control=1, *, selected=None):
    rows = field_tests.snapshot(population.field_population, control)
    selected = defias.RECRUIT_IDS if selected is None else set(selected)
    for actor in population.recruits:
        if actor.spawn_id in selected:
            rows.extend([
                dict(kind="agent", id=actor.spawn_id, spawn=actor.spawn_id, map=725,
                    type=actor.agent_type, control=control, home_map=725, home_x=actor.x,
                    home_y=actor.y, home_z=actor.z, home_o=actor.orientation, work_map=None, faction=2),
                dict(kind="participation", spawn=actor.spawn_id, mode=0),
                dict(kind="defias_work", id=actor.spawn_id, spawn=actor.spawn_id,
                    work_map=None, work_x=None, work_y=None, work_z=None, work_o=None)])
    return rows


def frame():
    current = field_tests.frame()
    raider = next(actor for actor in current["agents"] if actor["spawn_id"] == 901055)
    for actor in current["agents"]:
        identity = actor["spawn_id"]
        if identity not in defias.RECRUIT_IDS:
            continue
        actor.update(native_creature=False, control_mode="AI_WORLD_CONTROLLED", needs={"hunger": .2}, economy={},
            type="GUARD" if identity in defias.GUARD_IDS else "CIVILIAN" if identity in defias.SUPPLIER_IDS else "COMBATANT",
            world_faction=2, living_role=dict(enabled=True, extensions_enabled=True,
                role=defias.RECRUIT_ROLES[identity], lifetime_ms=1))
        if identity in defias.RAIDER_IDS:
            actor["raid"] = copy.deepcopy(raider["raid"])
            actor["supply"] = copy.deepcopy(raider["supply"])
        else:
            actor["supply"] = copy.deepcopy(raider["supply"])
            actor["supply"].update(role="CONSUMER", cargo=0, capacity=0)
    return current


class PublishedDefiasTests(unittest.TestCase):
    def test_all_sixteen_keep_authored_roles_homes_types_and_unchanged_field_baseline(self):
        population = defias.Population.load()
        self.assertEqual(population.field_population, fields.Population.load())
        self.assertEqual((len(population.actors), len(population.recruits), len(defias.NATIVE_IDS)), (55, 15, 45))
        self.assertEqual(defias.RAIDER_IDS, frozenset(range(901050, 901060)))
        originals = {actor.spawn_id: actor for actor in population.settlement.actors}
        for actor in population.recruits:
            self.assertEqual(tuple(getattr(actor, key) for key in ("entry", "label", "role", "x", "y", "z", "orientation")),
                tuple(getattr(originals[actor.spawn_id], key) for key in ("entry", "label", "role", "x", "y", "z", "orientation")))
            self.assertEqual((actor.agent_type, actor.world_faction, actor.depot_id), (defias.RECRUIT_TYPES[actor.spawn_id], 2, 3))
        self.assertEqual(len(population.geometry_files), 61)

    def test_new_profile_is_opt_in_and_pauses_all_runtime_capabilities(self):
        population = defias.Population.load()
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": defias.PROFILE, "LAB_NPC_AGENTS_ENABLED": "1"})
        config = manage.config_overrides(settings, population)
        for flag in ("LivingDefias.Enabled", "LivingFieldsEnabled", "LivingRaidsEnabled", "LivingConsumersEnabled", "LivingSupplyEnabled"):
            self.assertEqual(config["AIWorld." + flag], "1")
            self.assertEqual(manage.config_overrides(replace(settings, wildlife_enabled=False), population)["AIWorld." + flag], "0")
        for flag in ("DefiasGroupAutoFormation", "DefiasGroupRegroupEnabled", "GroupCoordination", "DefiasGroupRoamEnabled", "DefiasGroupHuntEnabled"):
            self.assertEqual(config["AIWorld." + flag], "0")
        for profile in ("disabled", settlement.PROFILE, raids.PROFILE, fields.PROFILE):
            self.assertEqual(manage.config_overrides(manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile}))["AIWorld.LivingDefias.Enabled"], "0")
        world_setup.validate_profile(settings)
        with self.assertRaises(manage.SetupError): manage.config_overrides(settings, population.field_population)
        with patch.object(settlement, "ensure_world_offline"), patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(settings)
        self.assertIn("agent_id NOT IN (" + population.spawn_ids + ")", sql.call_args_list[-1].args[1])

    def test_whole_camp_telemetry_has_ten_raiders_and_six_ordinary_camp_consumers(self):
        counts = telemetry_contract.validate_snapshot(frame(), defias.PROFILE)
        self.assertEqual((counts["managed"], counts["native"], counts["live_roles"]), (55, 45, 55))
        with self.assertRaises(defias.ProfileError): telemetry_contract.validate_snapshot(frame(), fields.PROFILE)
        for identity, section, key, value in ((901044, "living_role", "role", "RAIDER"), (901048, "supply", "cargo", 1),
            (901050, "supply", "capacity", 3), (901059, "raid", "source_depot_id", 3)):
            candidate = frame()
            actor = next(actor for actor in candidate["agents"] if actor["spawn_id"] == identity)
            actor[section][key] = value
            with self.subTest(identity=identity, key=key), self.assertRaises(defias.ProfileError):
                telemetry_contract.validate_snapshot(candidate, defias.PROFILE)
        candidate = frame()
        actor = next(actor for actor in candidate["agents"] if actor["spawn_id"] == 901044)
        actor["raid"] = copy.deepcopy(next(actor for actor in candidate["agents"] if actor["spawn_id"] == 901055)["raid"])
        with self.assertRaises(defias.ProfileError): telemetry_contract.validate_snapshot(candidate, defias.PROFILE)

    def test_every_raider_can_select_every_village_but_must_share_the_actual_ledger(self):
        for identity in sorted(defias.RAIDER_IDS):
            for depot_id in (1, 4, 5, 6):
                candidate = frame()
                raider = next(actor for actor in candidate["agents"] if actor["spawn_id"] == identity)
                depot = (raider["raid"]["source_depot"] if depot_id == 1 else
                    next(actor["supply"]["depot"] for actor in candidate["agents"]
                        if (actor.get("supply") or {}).get("depot_id") == depot_id))
                raider["raid"].update(source_depot_id=depot_id, source_depot=copy.deepcopy(depot))
                with self.subTest(identity=identity, depot_id=depot_id):
                    telemetry_contract.validate_snapshot(candidate, defias.PROFILE)
        candidate = frame()
        next(actor for actor in candidate["agents"] if actor["spawn_id"] == 901059)["raid"]["camp_depot"]["stock"] = 1
        with self.assertRaises(defias.ProfileError): telemetry_contract.validate_snapshot(candidate, defias.PROFILE)


class DefiasPopulationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = field_tests.FieldPopulationTests("test_insert_only_bootstrap_preserves_parent_activation_guards_and_offline_phase")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.metadata, self.data = self.fixture.metadata, self.fixture.data
        sha = write_defias_metadata(self.metadata, self.fixture.population)
        patcher = patch.object(defias, "BASE_FIELDS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = defias.Population.load(self.metadata)

    def test_manifest_cannot_expand_reclassify_move_or_persist_new_runtime_state(self):
        self.assertEqual(defias.verify_data(self.data, self.metadata), self.population)
        original = single_return.load_json(self.metadata / defias.MANIFEST)
        for mutate in (lambda d: d["actors"].pop(), lambda d: d["actors"][0].update(agent_type=True),
            lambda d: d["actors"][0]["home"].update(x=0), lambda d: d["actors"][4].update(living_role="RAIDER"),
            lambda d: d.update(runtime_raid_memory_only=False), lambda d: d["owned_spawn_ids"].append(901001),
            lambda d: d["camp_depot"]["position"].update(z=10)):
            candidate = copy.deepcopy(original); mutate(candidate); write_json(self.metadata / defias.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(defias.ProfileError): defias.Population.load(self.metadata)
        write_json(self.metadata / defias.MANIFEST, original)

    def test_sql_readback_checks_every_new_identity_before_filtering(self):
        defias.validate_snapshot(snapshot(self.population), self.population)
        defias.validate_snapshot(snapshot(self.population, selected=()), self.population, control=None, bootstrap=True)
        for kind, key, value in (("agent", "id", 999), ("agent", "spawn", 901045), ("agent", "type", 5),
            ("agent", "faction", 1), ("agent", "home_x", 0), ("agent", "work_map", 725),
            ("defias_work", "work_o", 0), ("participation", "mode", 1)):
            rows = snapshot(self.population)
            row = next(row for row in rows if row["kind"] == kind and row.get("id", row.get("spawn")) == 901044)
            row[key] = value
            with self.subTest(kind=kind, key=key), self.assertRaises(defias.ProfileError):
                defias.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for identity in (901044, 901050, 901055, 901016):
            rows = snapshot(self.population); rows.append(dict(kind="membership", group=1, member=identity))
            with self.subTest(identity=identity), self.assertRaises(defias.ProfileError): defias.validate_snapshot(rows, self.population)
        with self.assertRaises(defias.ProfileError): fields.validate_snapshot(snapshot(self.population), self.population.field_population)

    def test_insert_only_offline_extension_preserves_every_parent_activation_guard(self):
        bootstrap, activation = defias.bootstrap_sql(self.population), defias.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 15)
        self.assertEqual(activation.count("UPDATE characters.ai_agents"), 1)
        for text in ("COUNT(*)=20", "COUNT(*)=3", "COUNT(*)=2", "COUNT(*)=12", "COUNT(*)=15", "@defias_population_ready=1",
                     "a.agent_type=0", "a.agent_type=1", "a.agent_type=5", "agent_id IN (" + self.population.spawn_ids):
            self.assertIn(text, activation)
        for sql in (bootstrap, activation, defias.deactivation_sql(self.population)):
            for forbidden in ("DELETE", "ALTER", "auth.", "UPDATE world.", "SET home_", "SET agent_type", "food=", "money=", "resource=", "INSERT INTO characters.ai_agent_group_members"):
                self.assertNotIn(forbidden, sql)
        reads = iter((snapshot(self.population, selected=()), snapshot(self.population, 0), snapshot(self.population)))
        def sql(settings, query):
            return "\n".join(json.dumps(row) for row in next(reads)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": defias.PROFILE})
        with patch.object(defias, "verify_data", return_value=self.population), patch.object(settlement, "ensure_world_offline") as guard, patch.object(manage, "lab_mysql", side_effect=sql):
            manage.bootstrap_defias(self.data, settings)
        self.assertGreaterEqual(guard.call_count, 3)

    def test_explicit_deactivation_pauses_all_fifty_five_and_retains_reservations(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": defias.PROFILE})
        replies = iter((snapshot(self.population), snapshot(self.population, 0)))
        def sql(settings, query):
            return "\n".join(json.dumps(row) for row in next(replies)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        with patch.object(defias, "verify_data", return_value=self.population), patch.object(settlement, "ensure_world_offline") as guard, patch.object(manage, "lab_mysql", side_effect=sql) as queries:
            manage.deactivate_defias(self.data, settings)
        self.assertEqual(guard.call_count, 2)
        self.assertIn("SET control_mode=0", queries.call_args_list[1].args[1])


if __name__ == "__main__": unittest.main()
