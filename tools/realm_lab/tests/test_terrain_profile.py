"""Terrain proof closure, exact identity transition and offline SQL boundaries."""
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import hunt_population as hunt, manage, run_scoped, single_return as base, terrain_profile as terrain
from tools.realm_lab.tests.test_single_return import fixture as flat_fixture
from tools.realm_lab.tests.test_hunt_population import snapshot
from tools.realm_lab.tests import test_hunt_population_100 as hundred_tests
from tools.realm_lab.tests import test_terrain_geometry as geometry_tests
from tools.realm_lab.tests.test_setup import environment

ROOT = Path(__file__).resolve().parents[3]


def write(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    metadata, data = flat_fixture(root)
    points = base.load_json(metadata / "test-points.json")
    points["source_revision"] = "v3"
    def plane(x, y):
        return 2 + 0.025 * (x - 266.667) + 0.015 * (y - 800)
    points["home"]["floor_z"] = plane(points["home"]["x"], points["home"]["y"])
    for point in points["return_points"]:
        point["floor_z"] = plane(point["x"], point["y"])
    write(metadata / "test-points.json", points)
    document = base.load_json(base.METADATA / "hunt-population-100.json")
    document.update(profile=terrain.PROFILE, source_revision="v3")
    for actor in document["agents"]:
        actor["home"]["z"] = plane(actor["home"]["x"], actor["home"]["y"])
    write(metadata / "hunt-population-terrain-100.json", document)
    population = terrain.load_population(metadata)
    models = ["oak.m2", "rock.m2", "hut.wmo", "gate.wmo"]
    # Native serialized BIH header, followed by four complete ModelSpawns.
    tree = terrain.VMAP_MAGIC + b"\1NODE" + struct.pack("<6fI3II4I", 0, 0, 0, 1000, 1000, 100,
                                                       3, 0, 0, 0, 4, 0, 1, 2, 3) + b"GOBJ"
    tile = terrain.VMAP_MAGIC + struct.pack("<I", 4)
    for index, name in enumerate(models):
        encoded = name.encode()
        tile += struct.pack("<IHI7f6fI", 5 if index < 2 else 4, 0, index + 1,
                            200, 800, 0, 0, 0, 0, 1, 190, 790, 0, 210, 810, 10, len(encoded))
        tile += encoded + struct.pack("<I", index)
    additions = {"vmaps/725.vmtree": tree, "vmaps/725_30_31.vmtile": tile}
    additions.update({f"vmaps/{name}.vmo": terrain.VMAP_MAGIC + b"WMOD" + struct.pack("<2I", 8, 0) for name in models})
    analytic_terrain = terrain.terrain_geometry.read_terrain(geometry_tests.tile_bytes(plane))
    additions["maps/7253130.map"] = geometry_tests.float_map_bytes(analytic_terrain)
    server = base.load_json(metadata / "server-manifest.json")
    server["files"] = [item for item in server["files"] if item["path"] not in additions]
    for name, content in additions.items():
        target = data / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        server["files"].append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    projections = [{"spawn_id": actor.spawn_id, "x": actor.x, "y": actor.y, "z": actor.z,
                    "projected_x": actor.x, "projected_y": actor.y, "projected_z": actor.z} for actor in population.actors]
    projections += [{"name": point["name"], "x": point["x"], "y": point["y"], "z": point["floor_z"],
                     "projected_x": point["x"], "projected_y": point["y"], "projected_z": point["floor_z"]} for point in points["return_points"]]
    routes = terrain.required_routes(population, points)
    server.update(source_revision="v3", client_patch_sha256="b" * 64,
                  custom_vmaps={"status": "verified_present", "objects": 4},
                  navigation={"ok": True, "scope": "navmesh_only", "map_id": 725, "projected_points": 104,
                              "actor_points": 100, "complete_paths": len(routes), "required_paths": len(routes),
                              "projections": projections,
                              "paths": [{"from": start, "to": end, "complete": True} for start, end in sorted(routes)]})
    write(metadata / "server-manifest.json", server)
    project = base.load_json(metadata / "project-manifest.json")
    project.update(source_revision="v3", geometry={"model_placements": 4, "vmap_instances": 4})
    project["client"]["patch"]["sha256"] = "b" * 64
    write(metadata / "project-manifest.json", project)
    return metadata, data, population


class TerrainProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data, self.population = fixture(Path(self.tmp.name))

    def mutate(self, name, change):
        document = base.load_json(self.metadata / name)
        change(document)
        write(self.metadata / name, document)

    def test_explicit_terrain_profile_preserves_all_hundred_identities_and_v2_guard(self):
        self.assertEqual(hunt.verify_data(self.data, self.metadata, profile=terrain.PROFILE), self.population)
        baseline = terrain.baseline_population()
        self.assertEqual(len(self.population.actors), 100)
        for old, new in zip(baseline.actors, self.population.actors):
            self.assertEqual((old.spawn_id, old.entry, old.role, old.label, old.x, old.y, old.orientation),
                             (new.spawn_id, new.entry, new.role, new.label, new.x, new.y, new.orientation))
        self.assertEqual(self.population.observer_environment(), baseline.observer_environment())
        with self.assertRaises(base.ProfileError):
            base.Scope.load(self.metadata)

    def test_last_actor_xy_role_or_duplicate_identity_drift_is_rejected(self):
        original = base.load_json(self.metadata / "hunt-population-terrain-100.json")
        for mutate in (lambda doc: doc["agents"][-1]["home"].update(x=doc["agents"][-1]["home"]["x"] + 1),
                       lambda doc: doc["agents"][-1].update(role="predator", entry=1186),
                       lambda doc: doc["agents"][-1].update(spawn_id=900823),
                       lambda doc: doc["agents"][-1]["home"].update(z=float("inf"))):
            value = deepcopy(original)
            mutate(value)
            write(self.metadata / "hunt-population-terrain-100.json", value)
            with self.assertRaises(base.ProfileError):
                terrain.load_population(self.metadata)

    def test_every_live_model_hash_and_binary_referenced_closure_is_required(self):
        model = self.data / "vmaps/oak.m2.vmo"
        original = model.read_bytes()
        model.unlink()
        with self.assertRaisesRegex(base.ProfileError, "Missing live terrain file"):
            terrain.verify_data(self.data, self.metadata)
        model.write_bytes(original + b"changed")
        with self.assertRaisesRegex(base.ProfileError, "differs from reviewed"):
            terrain.verify_data(self.data, self.metadata)
        model.write_bytes(original)
        self.mutate("server-manifest.json", lambda doc: doc["files"].remove(next(item for item in doc["files"] if item["path"] == "vmaps/oak.m2.vmo")))
        with self.assertRaisesRegex(base.ProfileError, "complete VMAP model closure"):
            terrain.verify_data(self.data, self.metadata)

    def test_navigation_input_heights_projection_and_exact_directed_routes_are_required(self):
        original = base.load_json(self.metadata / "server-manifest.json")
        for mutate in (lambda doc: doc["navigation"]["projections"][-1].update(z=100),
                       lambda doc: doc["navigation"]["projections"][99].update(projected_z=100),
                       lambda doc: doc["navigation"]["projections"][99].update(spawn_id=900823),
                       lambda doc: doc["navigation"]["paths"][-1].update(complete=False),
                       lambda doc: doc["navigation"]["paths"][-1].update(to="unknown")):
            value = deepcopy(original)
            mutate(value)
            write(self.metadata / "server-manifest.json", value)
            with self.assertRaises(base.ProfileError):
                terrain.verify_data(self.data, self.metadata)

    def test_coordinated_home_and_navigation_height_drift_still_fails_live_ground_check(self):
        self.mutate("hunt-population-terrain-100.json", lambda doc: doc["agents"][-1]["home"].update(
            z=doc["agents"][-1]["home"]["z"] + 0.5))
        def change_projection(document):
            point = next(point for point in document["navigation"]["projections"] if point.get("spawn_id") == 900824)
            point.update(z=point["z"] + 0.5, projected_z=point["projected_z"] + 0.5)
        self.mutate("server-manifest.json", change_projection)
        with self.assertRaisesRegex(base.ProfileError, "ground height differs.*900824"):
            terrain.verify_data(self.data, self.metadata)

    def test_rehashed_malformed_height_map_is_rejected_before_database_work(self):
        path = self.data / "maps/7253130.map"
        content = bytearray(path.read_bytes())
        height_offset, = struct.unpack_from("<I", content, 20)
        struct.pack_into("<I", content, height_offset + 4, 2)  # Unexpected packed-integer MHGT.
        path.write_bytes(content)
        def rehash(document):
            item = next(item for item in document["files"] if item["path"] == "maps/7253130.map")
            item["sha256"] = hashlib.sha256(content).hexdigest()
        self.mutate("server-manifest.json", rehash)
        with patch.object(manage, "lab_mysql") as sql:
            with self.assertRaisesRegex(base.ProfileError, "Invalid live extracted terrain ground"):
                manage.migrate_terrain(self.data, manage.Settings.load(environment()), self.metadata)
            sql.assert_not_called()

    def test_transition_accepts_only_whole_source_or_whole_target_and_rejects_foreign_entities(self):
        old = terrain.baseline_population()
        self.assertEqual(terrain.validate_transition_snapshot(snapshot(old, 1), self.population), "flat-v2")
        self.assertEqual(terrain.validate_transition_snapshot(snapshot(self.population, 1), self.population), "terrain-v3")
        for rows in (snapshot(old, 1), snapshot(self.population, 1)):
            next(row for row in rows if row["kind"] == "agent" and row["id"] == 900824)["home_z"] = 999
            with self.assertRaisesRegex(base.ProfileError, "drift/mixed"):
                terrain.validate_transition_snapshot(rows, self.population)
        extra = dict(next(row for row in snapshot(old, 1) if row["kind"] == "agent"), id=999, spawn=999, map=0, control=1)
        with self.assertRaises(base.ProfileError):
            terrain.validate_transition_snapshot(snapshot(old, 1) + [extra], self.population)
        mixed = snapshot(old, 1)
        next(row for row in mixed if row["kind"] == "spawn" and row["guid"] == 900725)["z"] = self.population.actors[0].z
        next(row for row in mixed if row["kind"] == "agent" and row["id"] == 900725)["home_z"] = self.population.actors[0].z
        with self.assertRaisesRegex(base.ProfileError, "drift/mixed"):
            terrain.validate_transition_snapshot(mixed, self.population)

    def test_startup_config_and_observer_use_explicit_terrain_profile(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": terrain.PROFILE})
        config = manage.config_overrides(settings, self.population)
        self.assertEqual(config["AIWorld.ScopeSpawnIds"], '"' + ",".join(map(str, range(900725, 900825))) + '"')
        self.assertEqual(config["AIWorld.NeedsHungerRatePerSecond"], "0.003")
        for feature in ("RemoteInferenceEnabled", "GroupCoordination", "RecoveryAdviceEnabled", "LivingRoleExtensionsEnabled"):
            self.assertEqual(config[f"AIWorld.{feature}"], "0")
        with patch.dict(run_scoped.os.environ, {"LAB_AI_PROFILE": terrain.PROFILE}), \
             patch.object(run_scoped.sys, "argv", ["run_scoped.py", "command"]), \
             patch.object(terrain, "load_scope", return_value=self.population.scope) as scope, \
             patch.object(run_scoped.os, "execvp") as execute:
            run_scoped.main()
            scope.assert_called_once()
            execute.assert_called_once_with("command", ["command"])

    def test_migration_disabled_and_preflight_failure_prevent_mutation(self):
        enabled = manage.Settings.load({**environment(), "LAB_AI_PROFILE": terrain.PROFILE})
        with patch.object(manage, "lab_mysql") as sql:
            with self.assertRaisesRegex(manage.SetupError, "disabled"):
                manage.migrate_terrain(self.data, enabled, self.metadata)
            sql.assert_not_called()
        settings = manage.Settings.load(environment())
        with patch.object(terrain, "verify_data", side_effect=base.ProfileError("Hash mismatch")), \
             patch.object(manage, "lab_mysql") as sql:
            with self.assertRaises(base.ProfileError):
                manage.migrate_terrain(self.data, settings, self.metadata)
            sql.assert_not_called()

    def test_terrain_startup_runs_the_new_proof_before_sql_or_server_execution(self):
        env = {**environment(), "LAB_AI_PROFILE": terrain.PROFILE, "WORLD_VIEWER_TELEMETRY_TOKEN": "test-token"}
        with patch.dict(manage.os.environ, env), \
             patch.object(manage.sys, "argv", ["manage.py", "run-world", "unused.conf", "worldserver"]), \
             patch.object(terrain, "verify_data", side_effect=base.ProfileError("Terrain proof mismatch")) as verify, \
             patch.object(manage, "lab_mysql") as sql, patch.object(manage.os, "execv") as execute:
            with self.assertRaises(SystemExit) as raised:
                manage.main()
            self.assertEqual(raised.exception.code, 2)
            verify.assert_called_once()
            sql.assert_not_called()
            execute.assert_not_called()

    def test_migration_rechecks_private_sql_readback_and_never_uses_auth(self):
        old = terrain.baseline_population()
        before = "\n".join(json.dumps(row) for row in snapshot(old, 1))
        after = "\n".join(json.dumps(row) for row in snapshot(self.population, 0))
        results = [type("Result", (), {"returncode": 0, "stdout": output})() for output in (before, "", after)]
        with patch.object(manage.subprocess, "run", side_effect=results) as run, patch.object(manage, "mysql") as auth:
            manage.migrate_terrain(self.data, manage.Settings.load(environment()), self.metadata)
            self.assertEqual(run.call_count, 3)
            auth.assert_not_called()
            for call in run.call_args_list:
                self.assertIn("--host=lab-mysql", call.args[0])
                self.assertNotIn("auth.", call.kwargs["input"])


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class TerrainProfileMySQLTests(unittest.TestCase):
    sql = hundred_tests.HuntPopulation100MySQLTests.sql

    def setUp(self):
        hundred_tests.HuntPopulation100MySQLTests.setUp(self)
        self.baseline = self.population
        self.metadata, self.data, self.population = fixture(Path(self.tmp.name) / "terrain")
        self.sql(hunt.bootstrap_sql(self.baseline))
        self.sql(hunt.activation_sql(self.baseline))

    def rows(self):
        return hunt.parse_snapshot(self.sql(hunt.snapshot_sql(self.population)))

    def test_atomic_hundred_height_transition_idempotence_and_activation_preserve_everything_else(self):
        self.sql("UPDATE world.creature SET curhealth=guid-900700,curmana=5 WHERE map=725;")
        query = "SELECT guid,id,map,position_x,position_y,orientation,StringId,curhealth,curmana FROM world.creature WHERE map=725 ORDER BY guid;"
        original = self.sql(query)
        terrain.validate_transition_snapshot(self.rows(), self.population)
        self.sql(terrain.transition_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=0)
        self.assertEqual(self.sql(query), original)
        self.sql(terrain.transition_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=0)
        self.sql(hunt.activation_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=1)
        self.assertEqual(self.sql("SELECT agent_type,COUNT(*) FROM characters.ai_agents WHERE control_mode=1 GROUP BY agent_type ORDER BY agent_type;"), "6\t20\n7\t80")
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.auth_tables)

    def test_sql_gate_refuses_partial_height_state_without_changing_any_other_actor(self):
        self.sql("UPDATE characters.ai_agents SET home_z=999 WHERE agent_id=900824;")
        query = "SELECT agent_id,home_z,control_mode FROM characters.ai_agents ORDER BY agent_id;"
        before = self.sql(query)
        self.sql(terrain.transition_sql(self.population))
        self.assertEqual(self.sql(query), before)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM world.creature WHERE map=725 AND position_z<>0;"), "0")
        self.sql("UPDATE characters.ai_agents SET home_z=0 WHERE agent_id=900824;")
        z = self.population.actors[0].z
        self.sql(f"UPDATE world.creature SET position_z={z} WHERE guid=900725; UPDATE characters.ai_agents SET home_z={z} WHERE agent_id=900725;")
        before = self.sql(query)
        self.sql(terrain.transition_sql(self.population))
        self.assertEqual(self.sql(query), before)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM world.creature WHERE map=725 AND position_z<>0;"), "1")

    def test_sql_gate_refuses_foreign_controlled_agent_and_unexpected_map_spawn(self):
        query = "SELECT agent_id,home_z,control_mode FROM characters.ai_agents ORDER BY agent_id;"
        self.sql("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);")
        before = self.sql(query)
        self.sql(terrain.transition_sql(self.population))
        self.assertEqual(self.sql(query), before)
        self.sql("DELETE FROM characters.ai_agents WHERE agent_id=999;")
        self.sql("INSERT INTO world.creature (guid,id,map) VALUES (999,1186,725);")
        before = self.sql(query)
        self.sql(terrain.transition_sql(self.population))
        self.assertEqual(self.sql(query), before)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM world.creature WHERE map=725 AND position_z<>0;"), "0")
