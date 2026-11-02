"""Residential homes, native ownership and independent template deployment."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates, manage, residents_population as residents, telemetry_contract
from tools.realm_lab.tests.test_setup import environment


def write_json(path, value):
    path.write_bytes((json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


def fixture(root):
    metadata = root / "preview-v10"
    metadata.mkdir()
    files = ({"path": "vmaps/farm.vmo", "bytes": 1, "sha256": "a" * 64},)
    geometry = SimpleNamespace(source_revision="preview-v10", metadata=metadata, geometry_files=files,
                               entry_proof={}, entry_proof_sha256="a" * 64)
    proof = dict(schema_version=1, profile=residents.PROFILE, source_revision="preview-v10", map_id=725,
                 ok=True, geometry_sha256=residents.geometry_digest(files), geometry_files=list(files),
                 native_navigation=dict(ok=True,map_id=725,poly_ref_bits=64,tiles=4,include_flags=1,exclude_flags=65534,
                                        valid_projections=23,required_paths=44,complete_paths=44), buildings=[])
    actors = []
    for index, house in enumerate(residents.HOUSE_IDS):
        positions = []
        for slot in (1, 2):
            spawn = 902000 + index * 2 + slot - 1
            route = dict(complete=True, all_corridor_flags_ground_only=True,
                         status=dict(succeeded=True, partial=False, buffer_too_small=False, out_of_nodes=False, failed=False))
            point = dict(slot=slot, x=100 + index * 10, y=100 + slot * 3, z=2.05, orientation=0,
                         geometry=dict(floor_z=2, slope_degrees=0, roof_clearance=3,
                                       clear_radius=.75, floor_circle_height_range=0, dry=True, indoor=True,
                                       full_native_scene_cylinder_clear=True, source_all_water_layers_checked=True,
                                       group_flags=0x2000), navigation=dict(valid=True, projection=dict(valid=True,flags=1,xy_delta=0,z_delta=0),
                                                                        entry_path=route, return_path=route))
            positions.append(point)
            actors.append(dict(spawn_id=spawn, entry=11328, house_id=house, slot=slot,
                               **{key: point[key] for key in ("x", "y", "z", "orientation")},
                               label=f"aiworld_lab_resident_{spawn}"))
        proof["buildings"].append(dict(house_id=house, mesh_file="farm.vmo", mesh_sha256="a" * 64, positions=positions))
    proof_path = metadata / residents.POSITION_PROOF
    write_json(proof_path, proof)
    document = dict(schema_version=1, profile=residents.PROFILE, realm_id=2, map_id=725, area_id=4988,
                    source_revision="preview-v10", geometry_sha256=residents.geometry_digest(files),
                    position_proof_sha256=hashlib.sha256(proof_path.read_bytes()).hexdigest(),
                    respawn_seconds=300, templates=[residents.BASE_TEMPLATE], actors=actors)
    write_json(metadata / residents.MANIFEST, document)
    return metadata, geometry, document, proof


def native_snapshot(population):
    return [{"kind": "template", **residents.BASE_TEMPLATE}] + [dict(
        kind="spawn", guid=actor.spawn_id, entry=actor.entry, equipment_id=-1, map=725, zone=4988, area=4988,
        spawn_mask=1, phase_mask=1, x=actor.x, y=actor.y, z=actor.z, orientation=actor.orientation,
        movement=0, wander=0, label=actor.label, script_name="", respawn=population.respawn_seconds,
        npcflag=0, unit_flags=0, dynamicflags=0) for actor in population.actors]


def telemetry(population):
    return dict(version=4, stale=False, captured_at_ms=12345, agents=[dict(
        spawn_id=a.spawn_id, agent_id=a.spawn_id, native_creature=True, control_mode="OBSERVE_ONLY",
        position=dict(map_id=725, x=a.x, y=a.y, z=a.z), needs=None, economy=None, living_role=None,
        supply=None, raid=None) for a in population.actors])


class ResidentPopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.geometry, self.document, self.proof = fixture(Path(self.tmp.name))
        self.population = residents.Population.load(self.metadata, self.geometry)
        self.settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": residents.PROFILE,
                                              "LAB_WILDLIFE_ENABLED": "0", "LAB_NPC_AGENTS_ENABLED": "0"})

    def test_two_separate_homes_per_reviewed_house_and_no_service_buildings(self):
        self.assertEqual(len(self.population.actors), 22)
        self.assertEqual({a.spawn_id for a in self.population.actors}, residents.SPAWN_IDS)
        self.assertEqual({a.house_id for a in self.population.actors}, set(residents.HOUSE_IDS))
        self.assertEqual(len({residents.expected_entry(a.spawn_id) for a in self.population.actors}), 22)
        self.assertEqual(residents.expected_entry(902000), 9810000)
        self.assertEqual(residents.expected_entry(902021), 9810021)
        self.assertIs(self.population.settlement, self.population)
        for house in residents.HOUSE_IDS:
            self.assertEqual([a.slot for a in self.population.actors if a.house_id == house], [1, 2])

    def test_identity_geometry_and_house_allocation_drift_are_refused(self):
        for field, value in (("profile", "full-v8"), ("source_revision", "preview-v9"), ("realm_id", 3),
                             ("map_id", 0), ("geometry_sha256", "0" * 64), ("position_proof_sha256", "0" * 64)):
            write_json(self.metadata / residents.MANIFEST, {**self.document, field: value})
            with self.subTest(field=field), self.assertRaises(residents.ProfileError):
                residents.Population.load(self.metadata, self.geometry)
        for field, value in (("house_id", 209), ("slot", 2), ("spawn_id", 901014), ("entry", 295), ("x", 150)):
            document = copy.deepcopy(self.document)
            document["actors"][0][field] = value
            write_json(self.metadata / residents.MANIFEST, document)
            with self.subTest(actor_field=field), self.assertRaises(residents.ProfileError):
                residents.Population.load(self.metadata, self.geometry)

    def test_changed_floor_or_failed_navigation_cannot_be_re_pinned_as_a_valid_point(self):
        for field, value in (("roof_clearance", 1), ("clear_radius", .1), ("slope_degrees", 45),
                             ("floor_z", 20), ("floor_circle_height_range", 2)):
            proof = copy.deepcopy(self.proof)
            proof["buildings"][0]["positions"][0]["geometry"][field] = value
            write_json(self.metadata / residents.POSITION_PROOF, proof)
            document = {**self.document, "position_proof_sha256": hashlib.sha256((self.metadata / residents.POSITION_PROOF).read_bytes()).hexdigest()}
            write_json(self.metadata / residents.MANIFEST, document)
            with self.subTest(field=field), self.assertRaises(residents.ProfileError):
                residents.Population.load(self.metadata, self.geometry)
        proof = copy.deepcopy(self.proof)
        proof["native_navigation"]["ok"] = False
        write_json(self.metadata / residents.POSITION_PROOF, proof)
        write_json(self.metadata / residents.MANIFEST, {**self.document, "position_proof_sha256": hashlib.sha256((self.metadata / residents.POSITION_PROOF).read_bytes()).hexdigest()})
        with self.assertRaises(residents.ProfileError): residents.Population.load(self.metadata, self.geometry)

    def test_runtime_disables_simulation_but_scopes_native_telemetry_to_exact_residents(self):
        flags = manage.config_overrides(self.settings, self.population)
        for key in ("AIWorld.Enable", "AIWorld.NpcAgentEnabled", "AIWorld.LivingRolesEnabled", "AIWorld.LivingCaravans.Enabled"):
            self.assertEqual(flags[key], "0", key)
        self.assertEqual(flags["AIWorld.TelemetryNativeCreaturesEnabled"], "1")
        self.assertEqual(flags["AIWorld.TelemetryEnabled"], "1")
        self.assertEqual(flags["AIWorld.ScopeSpawnIds"], '"' + self.population.spawn_ids + '"')
        self.assertEqual(flags["AIWorld.ScopeMinX"], str(residents.settlement.ACTIVE_BOUNDS["min_x"]))
        with self.assertRaises(manage.SetupError):
            manage.Settings.load({**environment(), "LAB_AI_PROFILE": residents.PROFILE, "LAB_NPC_AGENTS_ENABLED": "1"})

    def test_native_snapshot_rejects_foreign_homes_legacy_state_and_movement(self):
        rows = native_snapshot(self.population)
        residents.validate_snapshot(rows, self.population)
        residents.validate_snapshot(rows[:1], self.population, bootstrap=True)
        with self.assertRaises(residents.ProfileError): residents.validate_snapshot(rows[:1], self.population)
        for change in (dict(guid=901014), dict(entry=9810000), dict(x=1234), dict(movement=1), dict(spawn_mask=0)):
            altered = copy.deepcopy(rows)
            altered[1].update(change)
            with self.subTest(change=change), self.assertRaises(residents.ProfileError):
                residents.validate_snapshot(altered, self.population, bootstrap=True)
        for kind in ("agent", "participation", "membership", "memory", "smart_script", "spawn_addon", "moving_template"):
            with self.subTest(kind=kind), self.assertRaises(residents.ProfileError):
                residents.validate_snapshot([*rows, {"kind": kind}], self.population)

    def test_bootstrap_only_inserts_absent_exact_spawn_ids_and_never_rewrites_homes(self):
        sql = residents.bootstrap_sql(self.population)
        self.assertEqual(sql.count("INSERT INTO world.creature"), 22)
        self.assertEqual(sql.count("WHERE NOT EXISTS"), 22)
        for prohibited in ("DELETE", "UPDATE", "IGNORE", "characters.", "auth."):
            self.assertNotIn(prohibited, sql)
        self.assertIn("902000,11328,-1,725,4988,4988,1,1", sql)

    def test_startup_requires_all_owned_clones_and_normalizes_only_verified_bindings(self):
        specs = tuple(SimpleNamespace(spawn_id=a.spawn_id, entry=residents.expected_entry(a.spawn_id), base_entry=a.entry)
                      for a in self.population.actors)
        rows = native_snapshot(self.population)
        for row in rows[1:]: row["entry"] = residents.expected_entry(row["guid"])
        with patch.object(residents, "verify_data", return_value=self.population), \
             patch.object(character_templates, "verify", return_value=specs), \
             patch.object(manage, "lab_mysql", return_value="\n".join(json.dumps(row) for row in rows)):
            self.assertIs(manage.residents_preflight(Path("data"), self.settings), self.population)
        with patch.object(residents, "verify_data", return_value=self.population), \
             patch.object(character_templates, "verify", return_value=specs[:-1]):
            with self.assertRaises(manage.SetupError): manage.residents_preflight(Path("data"), self.settings)
        with self.assertRaises(manage.SetupError):
            manage.residents_preflight(Path("data"), replace(self.settings, ai_profile="authoring-empty"))

    def test_native_telemetry_requires_exact_22_population_without_simulation(self):
        state = telemetry(self.population)
        self.assertEqual(residents.validate_telemetry(state, self.population), dict(total=22, native=22, managed=0, live_roles=0))
        with patch.object(residents.Population, "load", return_value=self.population):
            self.assertEqual(telemetry_contract.validate_snapshot(state, residents.PROFILE)["native"], 22)
        for mutation in (lambda s: s.update(stale=True), lambda s: s["agents"].pop(),
                         lambda s: s["agents"][0].update(native_creature=False),
                         lambda s: s["agents"][0]["position"].update(x=999),
                         lambda s: s["agents"][0].update(living_role={"enabled": True})):
            changed = copy.deepcopy(state)
            mutation(changed)
            with self.assertRaises(residents.ProfileError): residents.validate_telemetry(changed, self.population)

    def test_repeated_offline_bootstrap_accepts_already_owned_template_bindings(self):
        specs = tuple(SimpleNamespace(spawn_id=a.spawn_id, entry=residents.expected_entry(a.spawn_id), base_entry=a.entry)
                      for a in self.population.actors)
        rows = native_snapshot(self.population)
        for row in rows[1:]: row["entry"] = residents.expected_entry(row["guid"])
        with patch.object(manage, "residents_preflight", return_value=self.population), \
             patch.object(residents.settlement, "ensure_world_offline") as offline, \
             patch.object(character_templates, "verify", return_value=specs), \
             patch.object(manage, "lab_mysql", side_effect=["", "\n".join(json.dumps(row) for row in rows)]) as query:
            manage.bootstrap_residents(Path("data"), self.settings)
        self.assertEqual(offline.call_count, 3)
        self.assertEqual(query.call_count, 2)
        self.assertEqual(query.call_args_list[0].args[1], residents.bootstrap_sql(self.population))


if __name__ == "__main__": unittest.main()
