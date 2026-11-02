"""The v11 control migration runs only against disposable CI MySQL."""
import hashlib
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import living_v11 as living, residents_population as old, residents_v11 as native, single_return
from tools.realm_lab.tests.test_residents_population import write_json
from tools.realm_lab.tests import test_residents_v11_mysql as native_mysql


@unittest.skipUnless(os.getenv('LAB_TEST_MYSQL')=='1','Disposable MySQL integration is a CI gate')
class LivingV11MySQLTests(unittest.TestCase):
    def setUp(self):
        native_mysql.V11ResidentsMySQLTests.setUp(self)
        self.native=self.population
        self.sql(native.bootstrap_sql(self.native))
        self.population=living.Population(self.native)
        self.sql('''DROP TABLE characters.ai_agents;
CREATE TABLE characters.ai_agents (
 agent_id BIGINT PRIMARY KEY,agent_type INT,map_id INT,spawn_id BIGINT UNIQUE,control_mode INT,
 home_map_id INT,home_x FLOAT,home_y FLOAT,home_z FLOAT,home_o FLOAT,
 work_map_id INT NULL,work_x FLOAT NULL,work_y FLOAT NULL,work_z FLOAT NULL,work_o FLOAT NULL,
 world_faction_id INT,money INT DEFAULT 0,food INT DEFAULT 0,resource INT DEFAULT 0);
ALTER TABLE world.ai_spawn_participation_defaults ADD PRIMARY KEY (spawn_id);
DROP TABLE IF EXISTS characters.characters;
CREATE TABLE characters.characters (guid BIGINT PRIMARY KEY,name VARCHAR(80),position_x FLOAT);
INSERT INTO characters.characters VALUES (7,'Player',123.5);
INSERT INTO world.creature (guid,id,map,StringId,position_x) VALUES (42,11328,0,'untouched',123.5);
''')

    def sql(self,statement): return native_mysql.V11ResidentsMySQLTests.sql(self,statement)
    def snapshot(self): return single_return.parse_snapshot(self.sql(living.snapshot_sql(self.population)))

    def test_idempotent_activation_preserves45homes_sources_othermap_and_player(self):
        before={table:self.sql('SELECT * FROM '+table+' ORDER BY 1;') for table in
            ('world.creature','world.creature_template','world.smart_scripts','characters.characters')}
        for _ in range(2):
            living.validate_snapshot(self.snapshot(),self.population,bootstrap=True)
            self.sql(living.bootstrap_sql(self.population))
            living.validate_snapshot(self.snapshot(),self.population)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;'),'45')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents WHERE agent_type=5 AND world_faction_id=2;'),'10')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_spawn_participation_defaults WHERE participation_mode=0;'),'45')
        for table,value in before.items():self.assertEqual(self.sql('SELECT * FROM '+table+' ORDER BY 1;'),value)

    def test_source_home_collision_partial_or_foreign_agent_never_adopted(self):
        self.sql('UPDATE world.creature SET position_x=position_x+10 WHERE guid=902000;')
        with self.assertRaises(living.ProfileError):living.validate_snapshot(self.snapshot(),self.population,bootstrap=True)
        self.sql(living.bootstrap_sql(self.population))
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents;'),'0')
        self.sql('UPDATE world.creature SET position_x=position_x-10 WHERE guid=902000;')
        self.sql('INSERT INTO characters.ai_agents (agent_id,spawn_id,map_id,control_mode) VALUES (902000,902001,0,0);')
        with self.assertRaises(living.ProfileError):living.validate_snapshot(self.snapshot(),self.population,bootstrap=True)
        self.sql(living.bootstrap_sql(self.population))
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents;'),'1')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_spawn_participation_defaults;'),'0')

    def test_post_preflight_custom_faction_or_smart_script_change_blocks_all_writes(self):
        living.validate_snapshot(self.snapshot(),self.population,bootstrap=True)
        self.sql('UPDATE world.ai_world_faction_entry_defaults SET world_faction_id=1 WHERE creature_entry=9810035;')
        self.sql(living.bootstrap_sql(self.population))
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents;'),'0')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_spawn_participation_defaults;'),'0')
        self.sql('UPDATE world.ai_world_faction_entry_defaults SET world_faction_id=2 WHERE creature_entry=9810035;')
        self.sql('UPDATE world.smart_scripts SET action_type=46 WHERE entryorguid=38;')
        self.sql(living.bootstrap_sql(self.population))
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents;'),'0')


if __name__=='__main__':unittest.main()
