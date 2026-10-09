from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, single_return as lab
from tools.realm_lab.tests.test_setup import environment

ROOT = Path(__file__).resolve().parents[3]


def dbc(fields, rows, strings=b"\0"):
    return struct.pack("<4s4I", b"WDBC", len(rows), fields, fields * 4, len(strings)) + b"".join(
        struct.pack(f"<{fields}I", *row) for row in rows) + strings


def fixture(root):
    metadata, data = root / "metadata", root / "data"
    metadata.mkdir()
    documents = {}
    for name in ("test-points.json", "project-manifest.json", "server-manifest.json", "in-game-validation.json"):
        documents[name] = json.loads((lab.METADATA / name).read_text(encoding="utf-8"))
    native = documents["in-game-validation.json"]
    native["npc_test"]["status"] = "confirmed_by_user"
    native["player_confirmation"]["all_four_return_points_checked"] = "pending"
    row = [0] * 66
    row[0], row[1], row[2], row[22], row[63] = 725, 1, 0, 4988, 2
    area = [0] * 36
    area[:4] = [4988, 725, 0, 3618]
    files = {
        "dbc/Map.dbc": dbc(66, [row], b"\0AIWorldLab\0"),
        "dbc/AreaTable.dbc": dbc(36, [area]),
        "dbc/Light.dbc": b"reviewed light fixture",
        "maps/7253130.map": b"reviewed terrain fixture",
        "mmaps/725.mmap": b"reviewed nav params fixture",
        "mmaps/7253130.mmtile": b"reviewed nav tile fixture",
    }
    manifest_files = []
    for name, content in files.items():
        path = data / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        manifest_files.append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    documents["server-manifest.json"]["files"] = manifest_files
    for name, document in documents.items():
        (metadata / name).write_text(json.dumps(document), encoding="utf-8")
    return metadata, data


def snapshot(scope, control=0):
    return [
        {"kind": "template", "entry": lab.ENTRY, "ai_name": "", "script_name": "",
         "regenerate_health": 1, "vehicle": 0, "npcflag": 0},
        {"kind": "spawn", "guid": lab.SPAWN_ID, "entry": lab.ENTRY, "map": scope.map_id,
         "zone": scope.area_id, "area": scope.area_id, "spawn_mask": 1, "phase_mask": 1,
         "x": scope.home_x, "y": scope.home_y, "z": scope.home_z, "orientation": 0,
         "movement": 0, "wander": 0, "label": lab.SPAWN_LABEL, "script_name": ""},
        {"kind": "agent", "id": lab.SPAWN_ID, "spawn": lab.SPAWN_ID, "map": scope.map_id,
         "type": 6, "control": control, "home_map": scope.map_id, "home_x": scope.home_x,
         "home_y": scope.home_y, "home_z": scope.home_z, "home_o": 0, "work_map": None, "faction": 0},
        {"kind": "participation", "spawn": lab.SPAWN_ID, "mode": 0},
        {"kind": "type", "entry": lab.ENTRY, "type": 6},
    ]


def update_metadata(metadata, filename, mutate):
    path = metadata / filename
    document = json.loads(path.read_text())
    mutate(document)
    path.write_text(json.dumps(document))


class SingleReturnDataTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data = fixture(Path(self.tmp.name))

    def test_verified_bundle_accepts_one_native_episode_without_claiming_four_directions(self):
        scope = lab.verify_data(self.data, self.metadata)
        self.assertEqual((scope.map_id, scope.area_id), (725, 4988))
        self.assertEqual((scope.home_x, scope.home_y, scope.home_z), (266.667, 800, 0))
        self.assertEqual(scope.config()["AIWorld.ScopeSpawnIds"], '"900725"')
        env = scope.observer_environment()
        self.assertEqual(env["WORLD_VIEWER_SCOPE_MAP_ID"], "725")
        self.assertEqual(json.loads(env["WORLD_VIEWER_SCOPE_BOUNDS"]), {
            "min_x": 166.667, "max_x": 366.667, "min_y": 700, "max_y": 900,
        })

    def test_each_live_file_is_hash_checked_before_sql(self):
        for path in (self.data / directory for directory in ("dbc", "maps", "mmaps")):
            for filename in path.iterdir():
                original = filename.read_bytes()
                with self.subTest(file=filename.name):
                    filename.write_bytes(original + b"changed")
                    with self.assertRaisesRegex(lab.ProfileError, "differs from reviewed bundle"):
                        lab.verify_data(self.data, self.metadata)
                    filename.write_bytes(original)

    def test_missing_native_proof_blocks_bootstrap_without_any_sql(self):
        update_metadata(self.metadata, "in-game-validation.json",
                        lambda doc: doc["npc_test"].update(status="prepared_not_executed"))
        with patch.object(manage, "lab_mysql") as sql:
            with self.assertRaisesRegex(lab.ProfileError, "Native player entry"):
                manage.bootstrap_single(self.data, manage.Settings.load(environment()), self.metadata)
            sql.assert_not_called()

    def test_revision_or_map_mismatch_and_bad_home_are_rejected(self):
        cases = (
            ("server-manifest.json", lambda doc: doc.update(source_revision="other")),
            ("server-manifest.json", lambda doc: doc.update(map_id=0)),
            ("test-points.json", lambda doc: doc["home"].update(x=300)),
            ("test-points.json", lambda doc: doc["home"].update(floor_z=1)),
        )
        for filename, mutate in cases:
            path = self.metadata / filename
            original = path.read_bytes()
            with self.subTest(file=filename):
                update_metadata(self.metadata, filename, mutate)
                with self.assertRaises(lab.ProfileError):
                    lab.verify_data(self.data, self.metadata)
                path.write_bytes(original)

    def test_additional_tile_or_vmap_is_rejected(self):
        for name in ("maps/7253030.map", "mmaps/7253030.mmtile", "vmaps/725.vmtree"):
            path = self.data / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"unexpected")
            with self.subTest(file=name), self.assertRaises(lab.ProfileError):
                lab.verify_data(self.data, self.metadata)
            path.unlink()

    def test_rehashed_wrong_map_binding_still_rejected(self):
        path = self.data / "dbc/Map.dbc"
        content = bytearray(path.read_bytes())
        struct.pack_into("<I", content, 20 + 22 * 4, 12)
        path.write_bytes(content)
        def rehash(document):
            item = next(item for item in document["files"] if item["path"] == "dbc/Map.dbc")
            item["sha256"] = hashlib.sha256(content).hexdigest()
        update_metadata(self.metadata, "server-manifest.json", rehash)
        with self.assertRaisesRegex(lab.ProfileError, "Wrong live Map.dbc"):
            lab.verify_data(self.data, self.metadata)


class SingleReturnDatabaseTests(unittest.TestCase):
    def setUp(self):
        self.scope = lab.Scope.load()
        self.settings = manage.Settings.load(environment())

    def test_empty_bootstrap_and_completed_observe_or_active_modes(self):
        lab.validate_snapshot(snapshot(self.scope)[:1], self.scope, control=None, bootstrap=True)
        lab.validate_snapshot(snapshot(self.scope), self.scope, control=0)
        lab.validate_snapshot(snapshot(self.scope, 1), self.scope, control=1)
        with self.assertRaises(lab.ProfileError):
            lab.validate_snapshot(snapshot(self.scope)[:1], self.scope, control=0)

    def test_collision_home_role_and_group_denied(self):
        changes = ((1, "guid", 10), (1, "map", 0), (1, "x", self.scope.home_x + 1),
                   (1, "movement", 1), (2, "id", 10), (2, "type", 4),
                   (2, "home_y", 0), (3, "mode", 1), (4, "type", 4))
        for index, field, value in changes:
            rows = snapshot(self.scope)
            rows[index][field] = value
            with self.subTest(field=field), self.assertRaises(lab.ProfileError):
                lab.validate_snapshot(rows, self.scope, control=None, bootstrap=True)
        rows = snapshot(self.scope) + [{"kind": "membership", "group": 1, "member": lab.SPAWN_ID}]
        with self.assertRaisesRegex(lab.ProfileError, "group membership"):
            lab.validate_snapshot(rows, self.scope, control=None, bootstrap=True)

    def test_outside_controlled_agent_denied_except_explicit_legacy_demotion_during_bootstrap(self):
        rows = snapshot(self.scope)
        outside = deepcopy(rows[2])
        outside.update(id=80335, spawn=80335, map=0, control=1)
        rows.append(outside)
        lab.validate_snapshot(rows, self.scope, control=None, bootstrap=True)
        with self.assertRaisesRegex(lab.ProfileError, "outside agent"):
            lab.validate_snapshot(rows, self.scope, control=None)
        outside.update(id=999, spawn=999)
        with self.assertRaisesRegex(lab.ProfileError, "outside agent"):
            lab.validate_snapshot(rows, self.scope, control=None, bootstrap=True)

    def test_partial_sql_readback_fails_closed(self):
        with self.assertRaises(lab.ProfileError):
            lab.validate_snapshot([{"kind": "spawn"}], self.scope, control=0)

    def test_bootstrap_and_activation_only_route_to_private_database(self):
        result = type("Result", (), {"returncode": 0, "stdout": "okay"})()
        with patch.object(manage.subprocess, "run", return_value=result) as run:
            manage.lab_mysql(self.settings, lab.bootstrap_sql(self.scope))
        args, kwargs = run.call_args
        self.assertIn("--host=lab-mysql", args[0])
        self.assertEqual(args[0][-1], "world")
        self.assertEqual(kwargs["env"]["MYSQL_PWD"], "world-pass")
        self.assertNotIn("auth-pass", repr(args))
        for sql in (lab.snapshot_sql(self.scope), lab.bootstrap_sql(self.scope), lab.activation_sql(self.scope)):
            self.assertNotIn("auth.", sql)
            self.assertNotIn("DELETE", sql)
            self.assertNotIn("ALTER", sql)
        self.assertIn("a.agent_id=900725", lab.activation_sql(self.scope))
        self.assertIn("other.agent_id IS NULL", lab.activation_sql(self.scope))
        self.assertIn("ABS(a.home_x-266.667)", lab.activation_sql(self.scope))

    def test_collision_fails_before_any_bootstrap_mutation(self):
        rows = snapshot(self.scope)
        rows[1]["entry"] = 1
        with patch.object(lab, "verify_data", return_value=self.scope), \
             patch.object(manage, "lab_snapshot", return_value=rows), \
             patch.object(manage, "lab_mysql") as sql:
            with self.assertRaisesRegex(lab.ProfileError, "collision"):
                manage.bootstrap_single(Path("unused"), self.settings)
            sql.assert_not_called()

    def test_bootstrap_observe_then_explicit_activation_with_readback(self):
        with patch.object(lab, "verify_data", return_value=self.scope), \
             patch.object(manage, "lab_snapshot", side_effect=[snapshot(self.scope)[:1], snapshot(self.scope)]), \
             patch.object(manage, "lab_mysql") as sql:
            manage.bootstrap_single(Path("unused"), self.settings)
            self.assertIn("control_mode=0", sql.call_args.args[1])
        with patch.object(lab, "verify_data", return_value=self.scope), \
             patch.object(manage, "lab_snapshot", side_effect=[snapshot(self.scope), snapshot(self.scope, 1)]), \
             patch.object(manage, "lab_mysql") as sql:
            manage.activate_single(Path("unused"), self.settings)
            self.assertIn("SET a.control_mode=1", sql.call_args.args[1])

    def test_enabled_profile_cannot_bootstrap_or_activate(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "single-return"})
        for function in (manage.bootstrap_single, manage.activate_single):
            with self.assertRaises(manage.SetupError):
                function(Path("unused"), settings)


class SingleReturnConfigTests(unittest.TestCase):
    def test_disabled_default_and_single_profile_have_no_test_spawn_or_inference(self):
        for profile in ("disabled", "single-return"):
            settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile})
            overrides = manage.config_overrides(settings)
            self.assertEqual(overrides["AIWorld.TestSpawnId"], "0")
            for flag in ("RemoteInferenceEnabled", "LivingNeedEvolutionEnabled", "GroupCoarseSimulationEnabled",
                         "LivingWolvesEnabled", "LivingRoleExtensionsEnabled", "RecoveryAdviceEnabled",
                         "DynamicTaskEnable", "CoalitionMaintenance", "GroupCoordination"):
                self.assertEqual(overrides[f"AIWorld.{flag}"], "0")
            self.assertEqual(overrides["Updates.EnableDatabases"], "6")
            self.assertEqual(overrides["AIWorld.Enable"], "1" if profile == "single-return" else "0")

    def test_single_profile_scope_and_observer_match_versioned_coordinates(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "single-return"})
        scope = lab.Scope.load()
        config = manage.config_overrides(settings, scope)
        self.assertEqual(config["AIWorld.ScopeMapId"], scope.observer_environment()["WORLD_VIEWER_SCOPE_MAP_ID"])
        self.assertEqual(config["AIWorld.ScopeZoneIds"], '"4988"')
        self.assertEqual(config["AIWorld.ScopeSpawnIds"], '"900725"')
        self.assertEqual(config["AIWorld.ScopeRestrictAgents"], "1")
        self.assertEqual(config["AIWorld.ScopeAlwaysActive"], "1")
        rendered = manage.render_config((ROOT / "deploy/worldserver.conf").read_text(), settings, scope)
        self.assertIn('AIWorld.ScopeSpawnIds = "900725"\n', rendered)
        self.assertIn("AIWorld.TestSpawnId = 0\n", rendered)

    def test_failed_enabled_preflight_prevents_worldserver_exec(self):
        with patch.dict(manage.os.environ, {**environment(), "LAB_AI_PROFILE": "single-return",
                                          "WORLD_VIEWER_TELEMETRY_TOKEN": "test-token"}), \
             patch.object(manage.sys, "argv", ["manage.py", "run-world", "unused.conf", "worldserver"]), \
             patch.object(manage, "single_preflight", side_effect=lab.ProfileError("Hash mismatch")), \
             patch.object(manage.os, "execv") as execute:
            with self.assertRaises(SystemExit) as raised:
                manage.main()
            self.assertEqual(raised.exception.code, 2)
            execute.assert_not_called()

    def test_unknown_profile_or_other_realm_is_rejected(self):
        for env in ({"LAB_AI_PROFILE": "auto"}, {"LAB_AI_PROFILE": "single-return", "LAB_REALM_ID": "3"}):
            with self.assertRaises(manage.SetupError):
                manage.Settings.load({**environment(), **env})


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class SingleReturnMySQLTests(unittest.TestCase):
    """Exercise actual MySQL syntax and state transitions only in disposable CI."""
    def setUp(self):
        self.scope = lab.Scope.load()
        self.admin = manage.Settings.load({
            **environment(), "TC_AUTH_DB_HOST": "127.0.0.1", "TC_AUTH_DB_USER": "root",
            "TC_AUTH_DB_PASSWORD": os.environ["LAB_TEST_MYSQL_ROOT_PASSWORD"],
        })
        self.sql("""CREATE DATABASE IF NOT EXISTS world;
CREATE DATABASE IF NOT EXISTS characters;
DROP TABLE IF EXISTS world.creature, world.creature_template,
 world.ai_spawn_participation_defaults, world.ai_agent_type_entry_defaults,
 characters.ai_agents, characters.ai_agent_group_members;
CREATE TABLE world.creature_template (
 entry INT PRIMARY KEY, AIName VARCHAR(64), ScriptName VARCHAR(64), RegenHealth INT, VehicleId INT, npcflag INT);
CREATE TABLE world.creature (
 guid BIGINT PRIMARY KEY, id INT, map INT, zoneId INT, areaId INT, spawnMask INT, phaseMask INT,
 position_x FLOAT, position_y FLOAT, position_z FLOAT, orientation FLOAT, wander_distance FLOAT,
 MovementType INT, StringId VARCHAR(64), VerifiedBuild INT, ScriptName VARCHAR(64) NOT NULL DEFAULT '');
CREATE TABLE world.ai_spawn_participation_defaults (spawn_id BIGINT PRIMARY KEY, participation_mode INT);
CREATE TABLE world.ai_agent_type_entry_defaults (creature_entry INT PRIMARY KEY, agent_type INT);
CREATE TABLE characters.ai_agents (
 agent_id BIGINT PRIMARY KEY, agent_type INT, map_id INT, spawn_id BIGINT, control_mode INT,
 home_map_id INT, home_x FLOAT, home_y FLOAT, home_z FLOAT, home_o FLOAT,
 work_map_id INT NULL, world_faction_id INT, UNIQUE(map_id,spawn_id));
CREATE TABLE characters.ai_agent_group_members (group_id BIGINT, member_agent_id BIGINT);
INSERT INTO world.creature_template VALUES (1186,'','',1,0,0);
INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,world_faction_id)
 VALUES (80335,6,0,80335,1,0);
""")
        self.auth_tables = self.sql("SHOW TABLES FROM auth;")

    def sql(self, sql):
        return manage.mysql(self.admin, sql)

    def rows(self):
        return lab.parse_snapshot(self.sql(lab.snapshot_sql(self.scope)))

    def test_repeat_bootstrap_then_activate_preserves_auth_and_only_controls_one_bear(self):
        lab.validate_snapshot(self.rows(), self.scope, control=None, bootstrap=True)
        self.sql(lab.bootstrap_sql(self.scope))
        lab.validate_snapshot(self.rows(), self.scope, control=0)
        self.sql(lab.bootstrap_sql(self.scope))
        lab.validate_snapshot(self.rows(), self.scope, control=0)
        self.sql(lab.activation_sql(self.scope))
        lab.validate_snapshot(self.rows(), self.scope, control=1)
        self.assertEqual(self.sql("SELECT agent_id FROM characters.ai_agents WHERE control_mode=1;"), "900725")
        self.assertEqual(self.sql("SELECT control_mode FROM characters.ai_agents WHERE agent_id=80335;"), "0")
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.auth_tables)

    def test_sql_activation_guard_refuses_changed_home_and_outside_agent(self):
        self.sql(lab.bootstrap_sql(self.scope))
        self.sql("UPDATE characters.ai_agents SET home_x=300 WHERE agent_id=900725;")
        self.sql(lab.activation_sql(self.scope))
        self.assertEqual(self.sql("SELECT control_mode FROM characters.ai_agents WHERE agent_id=900725;"), "0")
        self.sql(f"UPDATE characters.ai_agents SET home_x={self.scope.home_x} WHERE agent_id=900725;")
        self.sql("UPDATE characters.ai_agents SET control_mode=1 WHERE agent_id=80335;")
        self.sql(lab.activation_sql(self.scope))
        self.assertEqual(self.sql("SELECT control_mode FROM characters.ai_agents WHERE agent_id=900725;"), "0")


if __name__ == "__main__":
    unittest.main()
