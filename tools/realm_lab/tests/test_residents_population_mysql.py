"""Native resident insert/read-back against disposable CI MySQL only."""
import os
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import manage, residents_population as residents, single_return
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_residents_population import fixture


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class ResidentPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        metadata, geometry, _, _ = fixture(Path(self.tmp.name))
        self.population = residents.Population.load(metadata, geometry)
        self.admin = manage.Settings.load({**environment(), "LAB_AI_PROFILE": residents.PROFILE,
            "TC_AUTH_DB_HOST": "127.0.0.1", "TC_AUTH_DB_USER": "root",
            "TC_AUTH_DB_PASSWORD": os.environ["LAB_TEST_MYSQL_ROOT_PASSWORD"]})
        self.sql("""CREATE DATABASE IF NOT EXISTS world;
CREATE DATABASE IF NOT EXISTS characters;
DROP TABLE IF EXISTS world.creature,world.creature_template,world.smart_scripts,
 world.creature_template_addon,world.creature_addon,world.ai_spawn_participation_defaults,
 characters.ai_agents,characters.ai_agent_group_members,characters.ai_long_term_memories;
CREATE TABLE world.creature_template (
 entry BIGINT PRIMARY KEY,AIName VARCHAR(64),ScriptName VARCHAR(64),RegenHealth INT,VehicleId INT,
 npcflag BIGINT,unit_flags BIGINT,unit_flags2 BIGINT,faction INT,minlevel INT,maxlevel INT,flags_extra BIGINT);
INSERT INTO world.creature_template VALUES (11328,'','',1,0,0,0,2048,12,6,7,2);
CREATE TABLE world.creature (
 guid BIGINT PRIMARY KEY,id BIGINT,equipment_id INT DEFAULT 0,map INT,zoneId INT,areaId INT,spawnMask INT,phaseMask INT,
 position_x FLOAT,position_y FLOAT,position_z FLOAT,orientation FLOAT,spawntimesecs INT DEFAULT 300,
 wander_distance FLOAT,MovementType INT,curhealth INT DEFAULT 1,curmana INT DEFAULT 0,
 StringId VARCHAR(64),VerifiedBuild INT DEFAULT 12340,ScriptName VARCHAR(64) NOT NULL DEFAULT '',
 npcflag BIGINT DEFAULT 0,unit_flags BIGINT DEFAULT 0,dynamicflags BIGINT DEFAULT 0);
CREATE TABLE world.smart_scripts (entryorguid BIGINT,source_type INT,id INT);
CREATE TABLE world.creature_template_addon (entry BIGINT,path_id BIGINT,mount BIGINT,StandState INT);
INSERT INTO world.creature_template_addon VALUES (11328,0,0,0);
CREATE TABLE world.creature_addon (guid BIGINT);
CREATE TABLE world.ai_spawn_participation_defaults (spawn_id BIGINT,participation_mode INT);
CREATE TABLE characters.ai_agents (agent_id BIGINT,spawn_id BIGINT,map_id INT,control_mode INT);
CREATE TABLE characters.ai_agent_group_members (group_id BIGINT,member_agent_id BIGINT);
CREATE TABLE characters.ai_long_term_memories (memory_id BIGINT,agent_id BIGINT);
""")

    def sql(self, statement): return manage.mysql(self.admin, statement)

    def snapshot(self): return single_return.parse_snapshot(self.sql(residents.snapshot_sql(self.population)))

    def test_repeatable_bootstrap_preserves_unrelated_creatures_and_native_source(self):
        self.sql("INSERT INTO world.creature (guid,id,map,StringId,position_x) VALUES (42,11328,0,'untouched',123);")
        before = self.sql("SELECT * FROM world.creature WHERE guid=42;")
        source = self.sql("SELECT * FROM world.creature_template WHERE entry=11328;")
        for _ in range(2):
            residents.validate_snapshot(self.snapshot(), self.population, bootstrap=True)
            self.sql(residents.bootstrap_sql(self.population))
            residents.validate_snapshot(self.snapshot(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM world.creature WHERE map=725;"), "22")
        self.assertEqual(self.sql("SELECT * FROM world.creature WHERE guid=42;"), before)
        self.assertEqual(self.sql("SELECT * FROM world.creature_template WHERE entry=11328;"), source)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents;"), "0")

    def test_foreign_reserved_identity_and_post_install_home_drift_are_never_adopted(self):
        self.sql("INSERT INTO world.creature (guid,id,map,StringId) VALUES (902000,11328,0,'foreign');")
        with self.assertRaises(residents.ProfileError):
            residents.validate_snapshot(self.snapshot(), self.population, bootstrap=True)
        self.sql("DELETE FROM world.creature WHERE guid=902000;")
        self.sql(residents.bootstrap_sql(self.population))
        self.sql("UPDATE world.creature SET position_x=position_x+3 WHERE guid=902000;")
        before = self.sql("SELECT position_x FROM world.creature WHERE guid=902000;")
        with self.assertRaises(residents.ProfileError):
            residents.validate_snapshot(self.snapshot(), self.population, bootstrap=True)
        self.sql(residents.bootstrap_sql(self.population))
        self.assertEqual(self.sql("SELECT position_x FROM world.creature WHERE guid=902000;"), before)


if __name__ == "__main__": unittest.main()
