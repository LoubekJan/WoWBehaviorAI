"""Execute wildlife transactions only against disposable CI MySQL."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, settlement_population as settlement, wildlife_population as wildlife
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_wildlife_population import fixture, native_snapshot, write_json

ROOT = Path(__file__).resolve().parents[3]


def literal(value):
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return str(value)


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class WildlifePopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        metadata, _, manifest_sha, proof_sha = fixture(Path(self.tmp.name))
        for name, value in (("BASE_MANIFEST_SHA256", manifest_sha), ("BASE_PLACEMENT_PROOF_SHA256", proof_sha)):
            patcher = patch.object(wildlife, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.population = wildlife.Population.load(metadata)
        self.admin = manage.Settings.load({**environment(), "TC_AUTH_DB_HOST": "127.0.0.1",
                                          "TC_AUTH_DB_USER": "root",
                                          "TC_AUTH_DB_PASSWORD": os.environ["LAB_TEST_MYSQL_ROOT_PASSWORD"]})
        smart_columns = ",".join(f"`{name}` " + ("TEXT" if name == "comment" else
                                "DOUBLE" if name.startswith("target_") and name[-1] in "xyzo" else "BIGINT")
                                 for name in settlement.SMART_FIELDS)
        addon_columns = ",".join(f"`{name}` " + ("TEXT NULL" if name == "auras" else "BIGINT")
                                 for name in settlement.ADDON_FIELDS)
        self.sql(f"""CREATE DATABASE IF NOT EXISTS world;
CREATE DATABASE IF NOT EXISTS characters;
DROP TABLE IF EXISTS world.creature,world.creature_template,world.smart_scripts,
 world.creature_template_addon,world.creature_addon,
 world.ai_spawn_participation_defaults,world.ai_agent_type_entry_defaults,
 characters.ai_agents,characters.ai_agent_group_members,characters.ai_long_term_memories;
CREATE TABLE world.creature_template (
 entry BIGINT PRIMARY KEY,AIName VARCHAR(64),ScriptName VARCHAR(64),RegenHealth BIGINT,VehicleId BIGINT,
 npcflag BIGINT,unit_flags BIGINT,unit_flags2 BIGINT,faction BIGINT,minlevel BIGINT,maxlevel BIGINT,flags_extra BIGINT);
CREATE TABLE world.creature (
 guid BIGINT PRIMARY KEY,id BIGINT,equipment_id INT DEFAULT 0,map INT,zoneId INT,areaId INT,spawnMask INT,phaseMask INT,
 position_x FLOAT,position_y FLOAT,position_z FLOAT,orientation FLOAT,spawntimesecs INT DEFAULT 300,
 wander_distance FLOAT,MovementType INT,curhealth INT DEFAULT 1,curmana INT DEFAULT 0,
 StringId VARCHAR(64),VerifiedBuild INT DEFAULT 12340,ScriptName VARCHAR(64) NOT NULL DEFAULT '',
 npcflag BIGINT DEFAULT 0,unit_flags BIGINT DEFAULT 0,dynamicflags BIGINT DEFAULT 0);
CREATE TABLE world.smart_scripts ({smart_columns});
CREATE TABLE world.creature_template_addon ({addon_columns});
CREATE TABLE world.creature_addon (guid BIGINT PRIMARY KEY);
CREATE TABLE world.ai_spawn_participation_defaults (spawn_id BIGINT PRIMARY KEY,participation_mode INT);
CREATE TABLE world.ai_agent_type_entry_defaults (creature_entry BIGINT PRIMARY KEY,agent_type INT);
CREATE TABLE characters.ai_agents (
 agent_id BIGINT PRIMARY KEY,agent_type INT,map_id INT,spawn_id BIGINT,control_mode INT,
 home_map_id INT,home_x FLOAT,home_y FLOAT,home_z FLOAT,home_o FLOAT,
 work_map_id INT NULL,work_x FLOAT NULL,work_y FLOAT NULL,work_z FLOAT NULL,work_o FLOAT NULL,
 world_faction_id INT,money INT DEFAULT 0,food INT DEFAULT 0,resource INT DEFAULT 0,UNIQUE(map_id,spawn_id));
CREATE TABLE characters.ai_agent_group_members (group_id BIGINT,member_agent_id BIGINT);
CREATE TABLE characters.ai_long_term_memories (
 memory_id BIGINT PRIMARY KEY,agent_id BIGINT,importance FLOAT,observation_count INT);
""")
        rows = native_snapshot(self.population.settlement)
        actors = {actor.spawn_id: actor for actor in self.population.settlement.actors}
        for row in rows:
            if row["kind"] == "spawn" and row["guid"] in actors:
                row["orientation"] = actors[row["guid"]].orientation
        self.insert_snapshot(rows)
        self.bind_legacy_readback(metadata, rows)
        self.before_native = self.sql("SELECT * FROM world.creature ORDER BY guid;")
        self.before_legacy = self.sql("SELECT * FROM characters.ai_agents ORDER BY agent_id;")
        self.auth_tables = self.sql("SHOW TABLES FROM auth;")

    def sql(self, sql):
        return manage.mysql(self.admin, sql)

    def rows(self):
        return wildlife.parse_snapshot(self.sql(wildlife.snapshot_sql(self.population)))

    def bind_legacy_readback(self, metadata, seeded):
        """Record fixture evidence as production does: from actual SQL JSON.

        MySQL's JSON representation of FLOAT zero/integral values need not
        preserve Python seed literals' int/float spelling. First verify every
        inserted legacy record semantically, then pin the exact SQL snapshot
        bytes through its canonical digest. Production hashes stay unchanged.
        """
        def index(rows):
            selected = {}
            identity_fields = {"spawn": "guid", "agent": "id", "participation": "spawn", "membership": "member"}
            for row in rows:
                kind = row["kind"]
                identity = row.get(identity_fields.get(kind, ""))
                if identity in settlement.LEGACY_IDS or (kind == "agent" and row["spawn"] in settlement.LEGACY_IDS):
                    key = (kind, identity)
                    self.assertNotIn(key, selected)
                    selected[key] = row
            return selected

        readback = self.rows()
        expected, actual = index(seeded), index(readback)
        self.assertEqual(set(actual), set(expected))
        for kind in ("spawn", "agent", "participation"):
            self.assertEqual({identity for row_kind, identity in actual if row_kind == kind}, settlement.LEGACY_IDS)
        coordinate_fields = {"x", "y", "z", "orientation", "home_x", "home_y", "home_z", "home_o"}
        for key, expected_row in expected.items():
            self.assertEqual(set(actual[key]), set(expected_row))
            for field, expected_value in expected_row.items():
                if field in coordinate_fields:
                    self.assertAlmostEqual(float(actual[key][field]), float(expected_value), delta=0.001,
                                           msg=f"Legacy SQL coordinate differs: {key} {field}")
                else:
                    self.assertEqual(actual[key][field], expected_value,
                                     f"Legacy SQL record differs: {key} {field}")
        self.legacy_readback_sha256 = settlement.legacy_snapshot_digest(readback)
        native_path = metadata / settlement.MANIFEST
        native = json.loads(native_path.read_text(encoding="utf-8"))
        native["legacy_snapshot_sha256"] = self.legacy_readback_sha256
        write_json(native_path, native)
        manifest_sha = hashlib.sha256(native_path.read_bytes()).hexdigest()
        overlay_path = metadata / wildlife.MANIFEST
        overlay = json.loads(overlay_path.read_text(encoding="utf-8"))
        overlay["settlement_manifest_sha256"] = manifest_sha
        write_json(overlay_path, overlay)
        # Only this disposable fixture's reviewed manifest changes. Loading it
        # again exercises the same production pin checks with its new evidence.
        patcher = patch.object(wildlife, "BASE_MANIFEST_SHA256", manifest_sha)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.population = wildlife.Population.load(metadata)

    def insert_snapshot(self, rows):
        mapping = {
            "template": ("world.creature_template", {"ai_name": "AIName", "script_name": "ScriptName",
                         "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}),
            "spawn": ("world.creature", {"entry": "id", "zone": "zoneId", "area": "areaId",
                      "spawn_mask": "spawnMask", "phase_mask": "phaseMask", "x": "position_x",
                      "y": "position_y", "z": "position_z", "movement": "MovementType",
                      "wander": "wander_distance", "label": "StringId", "script_name": "ScriptName",
                      "respawn": "spawntimesecs"}),
            "agent": ("characters.ai_agents", {"id": "agent_id", "spawn": "spawn_id", "map": "map_id",
                      "type": "agent_type", "control": "control_mode", "home_map": "home_map_id",
                      "work_map": "work_map_id", "faction": "world_faction_id"}),
            "participation": ("world.ai_spawn_participation_defaults", {"spawn": "spawn_id", "mode": "participation_mode"}),
            "smart_script": ("world.smart_scripts", {}),
            "template_addon": ("world.creature_template_addon", {}),
        }
        statements = ["SET SESSION sql_mode='NO_BACKSLASH_ESCAPES';"]
        for row in rows:
            table, columns = mapping[row["kind"]]
            keys = [key for key in row if key != "kind"]
            statements.append(f"INSERT INTO {table} (" + ",".join("`" + columns.get(key, key) + "`" for key in keys) +
                              ") VALUES (" + ",".join(literal(row[key]) for key in keys) + ");")
        self.sql("\n".join(statements))

    def prepare(self):
        wildlife.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(wildlife.bootstrap_sql(self.population))
        wildlife.validate_snapshot(self.rows(), self.population, control=0)

    def assert_owned_inactive(self):
        self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.spawn_ids});"), "0")

    def test_bootstrap_and_activation_execute_twice_without_resetting_native_or_persistent_state(self):
        self.prepare()
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31,control_mode=1 WHERE agent_id=901072;
INSERT INTO characters.ai_long_term_memories VALUES (1,901072,0.875,7);""")
        persistent = self.sql("SELECT money,food,resource FROM characters.ai_agents WHERE agent_id=901072;")
        memory = self.sql("SELECT * FROM characters.ai_long_term_memories;")
        self.sql(wildlife.bootstrap_sql(self.population))
        wildlife.validate_snapshot(self.rows(), self.population, control=None)
        for _ in range(2):
            self.sql(wildlife.activation_sql(self.population))
            wildlife.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "20")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE agent_id BETWEEN 901000 AND 901071 OR agent_id BETWEEN 901092 AND 901099;"), "0")
        self.assertEqual(self.sql("SELECT * FROM world.creature ORDER BY guid;"), self.before_native)
        self.assertEqual(self.sql("SELECT * FROM characters.ai_agents WHERE agent_id BETWEEN 900725 AND 900824 ORDER BY agent_id;"), self.before_legacy)
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), self.legacy_readback_sha256)
        self.assertEqual(self.sql("SELECT money,food,resource FROM characters.ai_agents WHERE agent_id=901072;"), persistent)
        self.assertEqual(self.sql("SELECT * FROM characters.ai_long_term_memories;"), memory)
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.auth_tables)

    def test_activation_sql_refuses_template_home_participation_group_and_native_drift(self):
        self.prepare()
        bear = next(actor for actor in self.population.actors if actor.spawn_id == 901086)
        native = self.population.settlement.actors[0]
        mutations = (
            ("UPDATE world.creature_template SET flags_extra=0 WHERE entry=883;", "UPDATE world.creature_template SET flags_extra=66 WHERE entry=883;"),
            ("UPDATE world.creature_template SET unit_flags2=0 WHERE entry=1186;", "UPDATE world.creature_template SET unit_flags2=2048 WHERE entry=1186;"),
            ("UPDATE world.creature_template SET AIName='SmartAI' WHERE entry=883;", "UPDATE world.creature_template SET AIName='' WHERE entry=883;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901086;", f"UPDATE characters.ai_agents SET home_x={bear.x} WHERE agent_id=901086;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901086;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901086;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901072;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901072;"),
            ("UPDATE world.ai_agent_type_entry_defaults SET agent_type=6 WHERE creature_entry=883;", "UPDATE world.ai_agent_type_entry_defaults SET agent_type=7 WHERE creature_entry=883;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901086);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901086;"),
            (f"UPDATE world.creature SET position_x=0 WHERE guid={native.spawn_id};", f"UPDATE world.creature SET position_x={native.x} WHERE guid={native.spawn_id};"),
        )
        for mutation, restore in mutations:
            with self.subTest(mutation=mutation):
                self.sql(mutation)
                with self.assertRaises(wildlife.ProfileError):
                    wildlife.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(wildlife.activation_sql(self.population))
                self.assert_owned_inactive()
                self.sql(restore)
        self.sql(wildlife.activation_sql(self.population))
        wildlife.validate_snapshot(self.rows(), self.population)

    def test_outside_controlled_agent_and_changed_masked_legacy_stop_activation(self):
        self.prepare()
        self.sql("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);")
        self.sql(wildlife.activation_sql(self.population))
        self.assert_owned_inactive()
        with self.assertRaises(wildlife.ProfileError):
            wildlife.validate_snapshot(self.rows(), self.population, control=None)
        self.sql("DELETE FROM characters.ai_agents WHERE agent_id=999; UPDATE characters.ai_agents SET control_mode=1 WHERE agent_id=900725;")
        self.sql(wildlife.activation_sql(self.population))
        self.assert_owned_inactive()
        with self.assertRaises(wildlife.ProfileError):
            wildlife.validate_snapshot(self.rows(), self.population, control=None)

    def test_recorded_legacy_digest_rejects_later_home_drift(self):
        self.prepare()
        self.sql("UPDATE characters.ai_agents SET home_x=home_x+0.25 WHERE agent_id=900725;")
        with self.assertRaisesRegex(wildlife.ProfileError, "Legacy regression identities/homes/control changed"):
            wildlife.validate_snapshot(self.rows(), self.population, control=None)


if __name__ == "__main__":
    unittest.main()
