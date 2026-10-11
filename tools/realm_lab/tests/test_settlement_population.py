"""Deployment guards for the static authored-map NPC population."""
import copy
import hashlib
import json
from pathlib import Path
import socket
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, run_scoped, settlement_population as settlement, single_return
from tools.realm_lab.tests.test_setup import environment

ROOT = Path(__file__).resolve().parents[3]


def template(entry):
    return {"entry": entry, "ai_name": "", "script_name": "", "regenerate_health": 1,
            "vehicle": 0, "npcflag": 0, "unit_flags": 0, "unit_flags2": 0,
            "faction": 12, "minlevel": 5, "maxlevel": 5, "flags_extra": 0}


def spawn(guid, entry, x, y, z, label, mask=1):
    return {"kind": "spawn", "guid": guid, "entry": entry, "equipment_id": -1 if mask == 1 else 0, "map": 725, "zone": 4988,
            "area": 4988, "spawn_mask": mask, "phase_mask": 1, "x": x, "y": y, "z": z,
            "orientation": 0.0, "movement": 0, "wander": 0, "label": label,
            "script_name": "", "respawn": 300, "npcflag": 0, "unit_flags": 0, "dynamicflags": 0}


def legacy_snapshot():
    rows = []
    for guid in sorted(settlement.LEGACY_IDS):
        label = "aiworld_lab_single_return" if guid == 900725 else f"aiworld_lab_hunt_{guid}"
        rows.append(spawn(guid, 1186, 250.0 + guid % 10, 800.0, 1.25, label, 0))
        rows.append({"kind": "agent", "id": guid, "spawn": guid, "map": 725, "type": 6,
                     "control": 0, "home_map": 725, "home_x": 250.0 + guid % 10,
                     "home_y": 800.0, "home_z": 1.25, "home_o": 0.0, "work_map": None, "faction": 0})
        rows.append({"kind": "participation", "spawn": guid, "mode": 0})
    return rows


def snapshot(population, *, actors=True):
    rows = (legacy_snapshot() + [{"kind": "template", **row} for row in population.templates] +
            list(population.smart_scripts) + list(population.template_addons))
    if actors:
        rows.extend(spawn(a.spawn_id, a.entry, a.x, a.y, a.z, a.label) for a in population.actors)
    return rows


def fixture(root):
    metadata, data = root / "metadata", root / "data"
    metadata.mkdir()
    paths = (["dbc/Map.dbc", "dbc/AreaTable.dbc", "dbc/Light.dbc"] +
             [f"maps/725{grid}.map" for grid in (3130, 3131, 3230, 3231)] + ["mmaps/725.mmap"] +
             [f"mmaps/725{grid}.mmtile" for grid in (3130, 3131, 3230, 3231)] + ["vmaps/725.vmtree"] +
             [f"vmaps/725_{grid}.vmtile" for grid in ("31_30", "31_31", "32_30", "32_31")] +
             [f"vmaps/model_{index:02}.vmo" for index in range(44)])
    files = []
    for name in paths:
        path = data / name
        path.parent.mkdir(parents=True, exist_ok=True)
        content = ("fixture:" + name).encode("ascii")
        path.write_bytes(content)
        files.append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    actors = []
    for entry, (faction, count) in enumerate(settlement.FACTION_COUNTS.items(), 100):
        for _ in range(count):
            offset = len(actors)
            actors.append({"spawn_id": 901000 + offset, "entry": entry, "faction": faction,
                           "role": "guard", "label": f"aiworld_lab_settlement_v8_{901000 + offset}",
                           "home": {"x": -400.0 + 10 * (offset % 10),
                                    "y": 100.0 + 10 * (offset // 10), "z": 1.0, "o": 0.0}})
    document = {"schema_version": 1, "profile": settlement.PROFILE, "realm_id": 2, "map_id": 725,
                "area_id": 4988, "source_revision": "preview-v8", "bounds": settlement.BOUNDS,
                "respawn_seconds": 300, "legacy_snapshot_sha256": settlement.legacy_snapshot_digest(legacy_snapshot()),
                "geometry_files": files, "templates": [template(entry) for entry in range(100, 106)], "actors": actors}
    document["smart_scripts"] = []
    document["template_addons"] = []
    proof = {"schema_version": 1, "status": "pass", "source_revision": "preview-v8", "map_id": 725,
             "geometry_files": files, "actors": [{"spawn_id": a["spawn_id"], "entry": a["entry"],
                                                   "home": a["home"], "dry": True, "native_ground": True,
                                                   "collision_clear": True} for a in actors]}
    proof_bytes = json.dumps(proof).encode("utf-8")
    (metadata / settlement.POSITION_PROOF).write_bytes(proof_bytes)
    document["placement_proof_sha256"] = hashlib.sha256(proof_bytes).hexdigest()
    (metadata / settlement.MANIFEST).write_text(json.dumps(document), encoding="utf-8")
    (metadata / "server-manifest.json").write_text(json.dumps({"schema_version": 1, "source_revision": "preview-v8",
                                                              "map_id": 725, "files": files}), encoding="utf-8")
    return metadata, data, document


class SettlementPopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data, self.document = fixture(Path(self.tmp.name))
        self.population = settlement.Population.load(self.metadata)

    def save(self, doc):
        (self.metadata / settlement.MANIFEST).write_text(json.dumps(doc), encoding="utf-8")

    def test_complete_native_closure_and_reserved_counts(self):
        self.assertEqual(settlement.verify_data(self.data, self.metadata), self.population)
        self.assertEqual({a.spawn_id for a in self.population.actors}, settlement.SPAWN_IDS)
        self.assertEqual(len(self.population.geometry_files), 61)
        settlement.validate_snapshot(snapshot(self.population), self.population)
        settlement.validate_snapshot(snapshot(self.population, actors=False), self.population, bootstrap=True)
        self.assertEqual(settlement.legacy_snapshot_digest(list(reversed(legacy_snapshot()))),
                         self.population.legacy_snapshot_sha256)

    def test_reserved_identity_role_counts_map_and_coordinate_mutations_fail(self):
        changes = [("realm_id", 1), ("map_id", 0), ("source_revision", "preview-v7")]
        for key, value in changes:
            bad = copy.deepcopy(self.document)
            bad[key] = value
            self.save(bad)
            with self.subTest(key=key), self.assertRaises(settlement.ProfileError):
                settlement.Population.load(self.metadata)
        for key, value in (("spawn_id", 900725), ("label", "foreign"), ("faction", "defias"), ("entry", True)):
            bad = copy.deepcopy(self.document)
            bad["actors"][0][key] = value
            self.save(bad)
            with self.subTest(key=key), self.assertRaises(settlement.ProfileError):
                settlement.Population.load(self.metadata)
        bad = copy.deepcopy(self.document)
        bad["actors"][0]["home"]["x"] = float("nan")
        self.save(bad)
        with self.assertRaises(settlement.ProfileError):
            settlement.Population.load(self.metadata)

    def test_geometry_modification_missing_collision_model_and_extra_custom_tile_fail(self):
        target = self.data / self.population.geometry_files[-1]["path"]
        old = target.read_bytes()
        target.write_bytes(b"corrupt")
        with self.assertRaisesRegex(settlement.ProfileError, "geometry hash mismatch"):
            settlement.verify_data(self.data, self.metadata)
        target.unlink()
        with self.assertRaises(settlement.ProfileError):
            settlement.verify_data(self.data, self.metadata)
        target.write_bytes(old)
        (self.data / "maps/7250000.map").write_bytes(b"unexpected")
        with self.assertRaisesRegex(settlement.ProfileError, "footprint"):
            settlement.verify_data(self.data, self.metadata)

    def test_home_and_native_proof_mutations_fail_before_database_work(self):
        doc = copy.deepcopy(self.document)
        doc["actors"][0]["home"]["x"] += 0.1
        self.save(doc)
        with self.assertRaisesRegex(settlement.ProfileError, "matching dry/native/collision"):
            settlement.Population.load(self.metadata)
        self.save(self.document)
        path = self.metadata / settlement.POSITION_PROOF
        proof = json.loads(path.read_text(encoding="utf-8"))
        proof["actors"][0]["dry"] = False
        contents = json.dumps(proof).encode("utf-8")
        path.write_bytes(contents)
        with self.assertRaisesRegex(settlement.ProfileError, "placement proof hash"):
            settlement.Population.load(self.metadata)
        doc = copy.deepcopy(self.document)
        doc["placement_proof_sha256"] = hashlib.sha256(contents).hexdigest()
        self.save(doc)
        with self.assertRaisesRegex(settlement.ProfileError, "matching dry/native/collision"):
            settlement.Population.load(self.metadata)

    def test_foreign_npc_template_changes_home_drift_and_legacy_activation_are_rejected(self):
        rows = snapshot(self.population)
        bad = copy.deepcopy(rows)
        bad.append(spawn(999, 100, 0, 500, 0, "foreign"))
        with self.assertRaisesRegex(settlement.ProfileError, "Foreign NPC"):
            settlement.validate_snapshot(bad, self.population, bootstrap=True)
        for kind, key, value in (("template", "ai_name", "SmartAI"), ("spawn", "spawn_mask", 1),
                                 ("agent", "control", 1), ("agent", "home_z", 9.0)):
            bad = copy.deepcopy(rows)
            row = next(row for row in bad if row["kind"] == kind)
            row[key] = value
            with self.subTest(kind=kind, key=key), self.assertRaises(settlement.ProfileError):
                settlement.validate_snapshot(bad, self.population, bootstrap=True)
        bad = copy.deepcopy(rows)
        next(row for row in bad if row.get("guid") == 901000)["x"] += 1
        with self.assertRaisesRegex(settlement.ProfileError, "home"):
            settlement.validate_snapshot(bad, self.population, bootstrap=True)
        bad = copy.deepcopy(rows)
        next(row for row in bad if row.get("guid") == 901000)["equipment_id"] = 0
        with self.assertRaisesRegex(settlement.ProfileError, "movement contract changed"):
            settlement.validate_snapshot(bad, self.population, bootstrap=True)
        bad = copy.deepcopy(rows)
        bad.append({"kind": "participation", "spawn": 901000, "mode": 0})
        with self.assertRaisesRegex(settlement.ProfileError, "participation"):
            settlement.validate_snapshot(bad, self.population)

    def test_partial_matching_spawns_are_repeatable_but_missing_legacy_is_never_adopted(self):
        rows = snapshot(self.population)
        rows = [r for r in rows if not (r.get("kind") == "spawn" and r.get("guid", 0) > 901049)]
        settlement.validate_snapshot(rows, self.population, bootstrap=True)
        with self.assertRaisesRegex(settlement.ProfileError, "bootstrap incomplete"):
            settlement.validate_snapshot(rows, self.population)
        rows = [r for r in rows if r.get("guid") != 900725]
        with self.assertRaisesRegex(settlement.ProfileError, "legacy population is incomplete"):
            settlement.validate_snapshot(rows, self.population, bootstrap=True)

    def test_sql_contains_only_guarded_new_world_inserts(self):
        sql = settlement.bootstrap_sql(self.population)
        self.assertEqual(sql.count("INSERT INTO world.creature"), 100)
        self.assertEqual(sql.count("WHERE NOT EXISTS"), 100)
        for forbidden in ("UPDATE", "DELETE", "auth.", "characters.", "creature_template", "900725", "ai_agents"):
            self.assertNotIn(forbidden, sql)
        self.assertIn("901000,100,-1,725,4988,4988,1,1", sql)
        self.assertTrue(sql.startswith("START TRANSACTION;") and sql.endswith("COMMIT;\n"))

    def test_native_smartai_is_pinned_including_timed_action_lists(self):
        doc = copy.deepcopy(self.document)
        doc["templates"][0]["ai_name"] = "SmartAI"
        row = {key: 0 for key in settlement.SMART_FIELDS}
        row.update(kind="smart_script", entryorguid=100, source_type=0, comment="native fixture",
                   action_type=80, action_param1=10000)
        timed = {**row, "entryorguid": 10000, "source_type": 9, "action_type": 1, "action_param1": 0}
        doc["smart_scripts"] = [row, timed]
        self.save(doc)
        population = settlement.Population.load(self.metadata)
        settlement.validate_snapshot(snapshot(population), population)
        bad = copy.deepcopy(snapshot(population))
        next(r for r in bad if r["kind"] == "smart_script")["action_type"] = 69
        with self.assertRaisesRegex(settlement.ProfileError, "SmartAI behavior"):
            settlement.validate_snapshot(bad, population)
        doc["smart_scripts"] = [row]
        self.save(doc)
        with self.assertRaisesRegex(settlement.ProfileError, "closure missing"):
            settlement.Population.load(self.metadata)

    def test_template_paths_and_spawn_addon_collisions_fail(self):
        doc = copy.deepcopy(self.document)
        addon = {key: 0 for key in settlement.ADDON_FIELDS}
        addon.update(kind="template_addon", entry=100, auras="5301")
        doc["template_addons"] = [addon]
        self.save(doc)
        population = settlement.Population.load(self.metadata)
        settlement.validate_snapshot(snapshot(population), population)
        bad = copy.deepcopy(snapshot(population))
        bad.append({"kind": "spawn_addon", "guid": 901000})
        with self.assertRaisesRegex(settlement.ProfileError, "foreign addon"):
            settlement.validate_snapshot(bad, population)
        doc["template_addons"][0]["path_id"] = 999
        self.save(doc)
        with self.assertRaisesRegex(settlement.ProfileError, "identity/path/emote"):
            settlement.Population.load(self.metadata)

    def test_offline_guard_fails_open_listener_and_unknown_network_state(self):
        connection = type("Connection", (), {"close": lambda self: None})()
        with patch.object(settlement.socket, "create_connection", return_value=connection), \
             self.assertRaisesRegex(settlement.ProfileError, "accepting connections"):
            settlement.ensure_world_offline()
        for error in (ConnectionRefusedError(), socket.gaierror(socket.EAI_NONAME, "No such host")):
            with patch.object(settlement.socket, "create_connection", side_effect=error) as connect:
                settlement.ensure_world_offline()
                connect.assert_called_once_with(("aitc-lab-worldserver-1", 8085), timeout=3)
        for error in (TimeoutError(), socket.gaierror(socket.EAI_AGAIN, "Temporary DNS failure"), socket.gaierror()):
            with patch.object(settlement.socket, "create_connection", side_effect=error), \
                 self.assertRaisesRegex(settlement.ProfileError, "Cannot confirm"):
                settlement.ensure_world_offline()
        with self.assertRaises(settlement.ProfileError):
            settlement.ensure_world_offline("original-worldserver")

    def test_settlement_profile_keeps_all_ai_and_telemetry_off(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": settlement.PROFILE})
        config = manage.config_overrides(settings, self.population)
        for key in ("Enable", "TelemetryEnabled", "LivingRolesEnabled", "LivingNeedEvolutionEnabled",
                    "ScopeAlwaysActive", "EnableSpawnReconciliation", "EnableZoneControlActivation"):
            self.assertEqual(config[f"AIWorld.{key}"], "0")
        self.assertEqual(config["Updates.EnableDatabases"], "6")
        with self.assertRaises(manage.SetupError):
            manage.Settings.load({**environment(), "LAB_AI_PROFILE": settlement.PROFILE, "LAB_REALM_ID": "3"})
        with patch.dict(run_scoped.os.environ, {"LAB_AI_PROFILE": settlement.PROFILE}), \
             patch.object(run_scoped.sys, "argv", ["run_scoped.py", "command"]), \
             patch.object(settlement.Population, "load", return_value=self.population), \
             patch.object(run_scoped.os, "execvp") as execute:
            run_scoped.main()
            self.assertEqual(json.loads(run_scoped.os.environ["WORLD_VIEWER_SCOPE_BOUNDS"]), settlement.BOUNDS)
            execute.assert_called_once_with("command", ["command"])

    def test_world_or_geometry_failure_prevents_every_sql_write(self):
        settings = manage.Settings.load(environment())
        with patch.object(settlement, "ensure_world_offline", side_effect=settlement.ProfileError("running")), \
             patch.object(manage, "lab_mysql") as private_sql, self.assertRaises(settlement.ProfileError):
            manage.bootstrap_settlement(self.data, settings, self.metadata)
        private_sql.assert_not_called()
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(settlement, "verify_data", side_effect=settlement.ProfileError("hash")), \
             patch.object(manage, "lab_mysql") as private_sql, self.assertRaises(settlement.ProfileError):
            manage.bootstrap_settlement(self.data, settings, self.metadata)
        private_sql.assert_not_called()

    def test_bootstrap_checks_offline_three_times_and_reads_back_exact_population(self):
        settings = manage.Settings.load(environment())
        output = lambda rows: "\n".join(json.dumps(row) for row in rows)
        responses = [output(snapshot(self.population, actors=False)), "", output(snapshot(self.population))]
        with patch.object(settlement, "ensure_world_offline") as guard, \
             patch.object(manage, "lab_mysql", side_effect=responses) as private_sql, \
             patch.object(manage, "mysql") as shared_auth:
            manage.bootstrap_settlement(self.data, settings, self.metadata)
            self.assertEqual(guard.call_count, 3)
            self.assertEqual(private_sql.call_count, 3)
            self.assertEqual(private_sql.call_args_list[1].args[1], settlement.bootstrap_sql(self.population))
            shared_auth.assert_not_called()

    def test_startup_geometry_failure_prevents_database_and_world_execution(self):
        with patch.dict(manage.os.environ, {**environment(), "LAB_AI_PROFILE": settlement.PROFILE}), \
             patch.object(manage.sys, "argv", ["manage.py", "run-world", "unused", "worldserver"]), \
             patch.object(settlement, "verify_data", side_effect=settlement.ProfileError("hash mismatch")), \
             patch.object(manage, "lab_mysql") as private_sql, \
             patch.object(manage.os, "execv") as execute:
            with self.assertRaises(SystemExit) as raised:
                manage.main()
            self.assertEqual(raised.exception.code, 2)
            private_sql.assert_not_called()
            execute.assert_not_called()


if __name__ == "__main__":
    unittest.main()
