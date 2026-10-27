"""Field ownership preserves the raid baseline and rejects incomplete evidence."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import field_population as fields, manage, raid_population as raids, single_return, telemetry_contract
from tools.realm_lab import settlement_population as settlement, world_setup
from tools.realm_lab.tests import test_raid_population as raid_tests
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_wildlife_population import write_json

ROOT = Path(__file__).resolve().parents[3]


def write_field_metadata(metadata):
    document = single_return.load_json(fields.METADATA / fields.MANIFEST)
    for filename in (fields.TEXTURE_AUDIT, fields.ASSIGNMENT_PROPOSAL):
        (metadata / filename).write_bytes((fields.METADATA / filename).read_bytes())
    document["raid_manifest_sha256"] = hashlib.sha256((metadata / raids.MANIFEST).read_bytes()).hexdigest()
    write_json(metadata / fields.MANIFEST, document)
    return document["raid_manifest_sha256"]


def write_navigation_fixture(metadata, population):
    """Synthetic fixture only; never a published navigation acceptance receipt."""
    anchors, routes = fields.navigation_requirements(population)
    success = {"succeeded": True, "failed": False, "detail_bits": 0}
    native = [row for row in population.geometry_files if row["path"].startswith(("mmaps/", "maps/"))]
    proof = dict(schema_version=1, profile=fields.PROFILE, source_revision="preview-v8", map_id=725,
        status="PASS", ok=True, scope="native_detour_ground_connectivity_only", physical_movement_verified=False,
        native_collision_queries_verified=False, native_geometry_files_verified=61, synthetic_test_fixture=True,
        population_manifest={"sha256": hashlib.sha256((metadata / fields.MANIFEST).read_bytes()).hexdigest()},
        native_navigation_inputs=[row for row in native if row["path"].startswith("mmaps/")],
        native_terrain_inputs=[row for row in native if row["path"].startswith("maps/")],
        results=dict(ok=True, map_id=725, poly_ref_bits=64, tiles=4, include_flags=1, exclude_flags=65534,
            projection_limits={"xy": 1, "z": 2}, valid_projections=len(anchors), required_paths=len(routes), complete_paths=len(routes),
            projections=[dict(id=key, input=point, projected=point, valid=True, status=success) for key, point in anchors.items()],
            paths=[dict(**{"from": start, "to": end}, complete=True, all_corridor_flags_ground_only=True,
                endpoint_delta=0, status=success, straight_status=success) for start, end in sorted(routes)]))
    write_json(metadata / fields.NAV_PROOF, proof)


def snapshot(population, control=1, *, selected=None):
    rows = raid_tests.snapshot(population.raids, control)
    selected = fields.RECRUIT_IDS if selected is None else set(selected)
    for actor in population.recruits:
        if actor.spawn_id in selected:
            rows.extend([
                dict(kind="agent", id=actor.spawn_id, spawn=actor.spawn_id, map=725, type=0,
                    control=control, home_map=725, home_x=actor.x, home_y=actor.y, home_z=actor.z,
                    home_o=actor.orientation, work_map=None, faction=1),
                dict(kind="participation", spawn=actor.spawn_id, mode=0),
                dict(kind="field_work", id=actor.spawn_id, spawn=actor.spawn_id,
                    work_map=None, work_x=None, work_y=None, work_z=None, work_o=None)])
    return rows


def frame():
    current = raid_tests.frame()
    population = fields.Population.load()
    actors = {actor.spawn_id: actor for actor in population.recruits}
    definitions = {field["id"]: field for field in population.fields}
    for actor in current["agents"]:
        farmer = actors.get(actor["spawn_id"])
        if farmer is None: continue
        definition = definitions[farmer.field_id]
        actor.update(native_creature=False, control_mode="AI_WORLD_CONTROLLED", needs={"hunger": .2}, economy={},
            type="CIVILIAN", world_faction=1,
            living_role=dict(enabled=True, extensions_enabled=True, role="FARMER", lifetime_ms=1))
        actor["supply"] = dict(enabled=True, role="FARMER", depot_id=farmer.depot_id, cargo=0, capacity=2,
            field_id=farmer.field_id, field_workpoint_index=farmer.worker_ordinal,
            field_workpoint={key: definition["work_points"][farmer.worker_ordinal][key] for key in ("map_id", "x", "y", "z")},
            field_remaining=24, field_capacity=24,
            depot=dict(id=farmer.depot_id, position=definition["warehouse"]["position"].copy(), stock=0, capacity=30, low_threshold=4),
            accounting=dict(conserved=True))
    for actor in current["agents"]:
        if isinstance(actor.get("supply"), dict):
            actor["supply"]["accounting"] = dict(initial_food="0", harvested="0", in_depots="0", in_cargo="0", consumed="0", lost="0", conserved=True)
    return current


class PublishedFieldTests(unittest.TestCase):
    def test_forty_managed_and_sixty_native_keep_all_homes_and_original_twenty_eight(self):
        population = fields.Population.load()
        self.assertEqual(population.raids, raids.Population.load())
        self.assertEqual((len(population.actors), len(fields.NATIVE_IDS)), (40, 60))
        self.assertEqual(len(population.geometry_files), 61)
        originals = {actor.spawn_id: actor for actor in population.settlement.actors}
        for actor in population.recruits:
            self.assertEqual((actor.entry, actor.label, actor.x, actor.y, actor.z, actor.orientation),
                tuple(getattr(originals[actor.spawn_id], key) for key in ("entry", "label", "x", "y", "z", "orientation")))
            self.assertEqual((actor.agent_type, actor.world_faction), (0, 1))
        anchors, routes = fields.navigation_requirements(population)
        self.assertEqual((len(anchors), len(routes)), (51, 132))
        fields.verify_navigation(population)
        for field in population.fields:
            self.assertEqual(len(field["work_points"]), 12)
            self.assertTrue(all(point["crop_alpha"] >= 128 and point["crop_boundary_clearance_yards"] >= 8 for point in field["work_points"]))

    def test_profile_opt_in_and_shared_pause(self):
        population = fields.Population.load()
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": fields.PROFILE})
        config = manage.config_overrides(settings, population)
        for flag in ("LivingRaidsEnabled", "LivingConsumersEnabled", "LivingSupplyEnabled", "LivingFieldsEnabled"):
            self.assertEqual(config["AIWorld." + flag], "1")
            self.assertEqual(manage.config_overrides(replace(settings, wildlife_enabled=False), population)["AIWorld." + flag], "0")
        for profile in ("disabled", settlement.PROFILE, raids.PROFILE):
            self.assertEqual(manage.config_overrides(manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile}))["AIWorld.LivingFieldsEnabled"], "0")
        with self.assertRaises(manage.SetupError): manage.config_overrides(settings, population.raids)
        world_setup.validate_profile(settings)
        with patch.object(settlement, "ensure_world_offline"), patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql:
            world_setup.demote_retired_pilots(settings)
        self.assertIn("agent_id NOT IN (" + population.spawn_ids + ")", sql.call_args_list[-1].args[1])

    def test_fresh_forty_role_telemetry_with_own_fields_and_bounded_food(self):
        counts = telemetry_contract.validate_snapshot(frame(), fields.PROFILE)
        self.assertEqual((counts["managed"], counts["native"], counts["live_roles"]), (40, 60, 40))
        for section, key, value in (("living_role", "role", "WORKER"), ("supply", "role", "HUNTER"),
            ("supply", "depot_id", 1), ("supply", "field_id", 5), ("supply", "cargo", 3),
            ("supply", "capacity", 3), ("supply", "field_workpoint_index", 1),
            ("supply", "field_remaining", 25), ("supply", "hunting_ground", {})):
            candidate = frame(); actor = next(actor for actor in candidate["agents"] if actor["spawn_id"] == 901016)
            actor[section][key] = value
            with self.subTest(key=key), self.assertRaises(fields.ProfileError): telemetry_contract.validate_snapshot(candidate, fields.PROFILE)
        with self.assertRaises(fields.ProfileError): telemetry_contract.validate_snapshot(frame(), raids.PROFILE)
        for key, value in (("in_cargo", "1"), ("harvested", 1), ("conserved", False)):
            candidate = frame()
            next(actor for actor in candidate["agents"] if actor["spawn_id"] == 901016)["supply"]["accounting"][key] = value
            with self.subTest(key=key), self.assertRaises(fields.ProfileError): telemetry_contract.validate_snapshot(candidate, fields.PROFILE)


class FieldPopulationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = raid_tests.RaidPopulationTests("test_sql_adds_only_two_reservations_and_setup_preserves_existing_twenty_six")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.metadata, self.data = self.fixture.metadata, self.fixture.data
        sha = write_field_metadata(self.metadata)
        patcher = patch.object(fields, "BASE_RAIDS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = fields.Population.load(self.metadata)
        write_navigation_fixture(self.metadata, self.population)

    def test_strict_manifest_and_native_proof_cannot_expand_move_or_omit_route(self):
        self.assertEqual(fields.verify_data(self.data, self.metadata), self.population)
        original = single_return.load_json(self.metadata / fields.MANIFEST)
        for mutate in (lambda d: d["recruits"].pop(), lambda d: d["recruits"][0].update(agent_type=True),
            lambda d: d["recruits"][0]["home"].update(x=0), lambda d: d["fields"][0]["warehouse"]["position"].update(x=0),
            lambda d: d["fields"][0]["work_points"][0].update(x=0), lambda d: d.update(harvest_work_ms=0),
            lambda d: d["fields"][0].update(initial_crop_potential=25)):
            candidate = copy.deepcopy(original); mutate(candidate); write_json(self.metadata / fields.MANIFEST, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(fields.ProfileError): fields.Population.load(self.metadata)
        write_json(self.metadata / fields.MANIFEST, original)
        proof = single_return.load_json(self.metadata / fields.NAV_PROOF)
        for mutate in (lambda d: d.update(status="INCONCLUSIVE"), lambda d: d["results"]["paths"].pop(),
            lambda d: d["results"].update(include_flags=65535),
            lambda d: d["results"]["paths"][0]["status"].update(detail_bits=1),
            lambda d: d["results"]["projections"][0]["projected"].update(z=20),
            lambda d: d["native_navigation_inputs"][0].update(sha256="0"*64)):
            candidate = copy.deepcopy(proof); mutate(candidate); write_json(self.metadata / fields.NAV_PROOF, candidate)
            with self.subTest(mutate=mutate), self.assertRaises(fields.ProfileError): fields.verify_data(self.data, self.metadata)
        (self.metadata / fields.NAV_PROOF).unlink()
        with self.assertRaises(fields.ProfileError): fields.verify_data(self.data, self.metadata)

    def test_recruit_rows_checked_before_filtering_and_all_work_columns_null(self):
        fields.validate_snapshot(snapshot(self.population), self.population)
        fields.validate_snapshot(snapshot(self.population, selected=()), self.population, control=None, bootstrap=True)
        for kind, key, value in (("agent", "id", 999), ("agent", "spawn", 901017), ("agent", "type", 5),
            ("agent", "control", 2), ("agent", "faction", 2), ("agent", "home_x", 0), ("agent", "work_map", 725),
            ("field_work", "work_x", 0), ("participation", "mode", 1)):
            rows = snapshot(self.population)
            row = next(row for row in rows if row["kind"] == kind and row.get("id", row.get("spawn")) == 901016)
            row[key] = value
            with self.subTest(kind=kind, key=key), self.assertRaises(fields.ProfileError): fields.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for sid in (901016, 901034, 901039, 901055):
            rows = snapshot(self.population)
            rows.append(dict(kind="membership", group=1, member=sid))
            with self.assertRaises(fields.ProfileError): fields.validate_snapshot(rows, self.population)
        with self.assertRaises(fields.ProfileError): raids.validate_snapshot(snapshot(self.population), self.population.raids)

    def test_insert_only_bootstrap_preserves_parent_activation_guards_and_offline_phase(self):
        bootstrap, activation = fields.bootstrap_sql(self.population), fields.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents"), 12)
        self.assertEqual(activation.count("UPDATE characters.ai_agents"), 1)
        for text in ("COUNT(*)=20", "COUNT(*)=3", "COUNT(*)=2", "COUNT(*)=12", "@field_population_ready=1",
                     "a.agent_type=5", "a.agent_type=1", "agent_id IN (" + self.population.spawn_ids):
            self.assertIn(text, activation)
        for sql in (bootstrap, activation, fields.deactivation_sql(self.population)):
            for forbidden in ("DELETE", "ALTER", "auth.", "UPDATE world.", "SET home_", "SET agent_type", "food=", "money=", "resource="):
                self.assertNotIn(forbidden, sql)
        reads = iter((snapshot(self.population, selected=()), snapshot(self.population, 0), snapshot(self.population)))
        def sql(settings, query):
            return "\n".join(json.dumps(row) for row in next(reads)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": fields.PROFILE})
        with patch.object(fields, "verify_data", return_value=self.population), patch.object(settlement, "ensure_world_offline") as guard, patch.object(manage, "lab_mysql", side_effect=sql):
            manage.bootstrap_fields(self.data, settings)
        self.assertGreaterEqual(guard.call_count, 3)

    def test_explicit_deactivation_preserves_reservations_and_verifies_all_forty_observe(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": fields.PROFILE})
        replies = iter((snapshot(self.population), snapshot(self.population, 0)))
        def sql(settings, query):
            return "\n".join(json.dumps(row) for row in next(replies)) if query.lstrip().startswith("SELECT JSON_OBJECT") else ""
        with patch.object(fields, "verify_data", return_value=self.population), patch.object(settlement, "ensure_world_offline") as guard, patch.object(manage, "lab_mysql", side_effect=sql) as queries:
            manage.deactivate_fields(self.data, settings)
        self.assertEqual(guard.call_count, 2)
        self.assertIn("SET control_mode=0", queries.call_args_list[1].args[1])


if __name__ == "__main__": unittest.main()
