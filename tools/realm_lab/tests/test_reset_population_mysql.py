"""Actual MySQL reset transaction, restricted to disposable CI databases."""
import os
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates as templates, full_population, manage, reset_population as reset
from tools.realm_lab.tests.test_character_templates import document
from tools.realm_lab.tests.test_setup import environment


@unittest.skipUnless(os.getenv('LAB_TEST_MYSQL') == '1', 'Disposable MySQL integration is a CI gate')
class ResetPopulationMySQLTests(unittest.TestCase):
    WORLD_TABLES = {'creature','creature_template',templates.LEDGER,'ai_spawn_participation_defaults',
                    'quest_template','vehicle_template_accessory','vehicle_accessory',
                    *(resource.table for resource in templates.RESOURCES),
                    *(table for database,table,_,_ in reset.SIDE_REFS if database=='world')}
    CHARACTER_TABLES = {'ai_agents','ai_long_term_memories','ai_agent_groups','ai_agent_group_members','respawn','characters'}

    def setUp(self):
        self.admin=manage.Settings.load({**environment(),'LAB_AI_PROFILE':'full-v8','TC_AUTH_DB_HOST':'127.0.0.1',
            'TC_AUTH_DB_USER':'root','TC_AUTH_DB_PASSWORD':os.environ['LAB_TEST_MYSQL_ROOT_PASSWORD']})
        self.settings=SimpleNamespace(realm_id=2,ai_profile='full-v8')
        self.sql('CREATE DATABASE IF NOT EXISTS world;CREATE DATABASE IF NOT EXISTS characters;')
        self.clean()
        self.sql('''CREATE TABLE world.creature_template (
entry INT PRIMARY KEY,name VARCHAR(100),subname VARCHAR(100),HealthModifier FLOAT,speed_run FLOAT,
lootid INT,NewFutureColumn VARCHAR(100)) ENGINE=InnoDB;
CREATE TABLE world.creature (guid INT PRIMARY KEY,id INT,map INT,StringId VARCHAR(64),position_x FLOAT,
position_y FLOAT,position_z FLOAT,orientation FLOAT,spawnMask INT,curhealth INT) ENGINE=InnoDB;
CREATE TABLE world.ai_spawn_participation_defaults (spawn_id BIGINT PRIMARY KEY,participation_mode INT) ENGINE=InnoDB;
CREATE TABLE world.creature_addon (guid INT PRIMARY KEY,path_id INT) ENGINE=InnoDB;
CREATE TABLE world.npc_vendor (entry INT,item INT,PRIMARY KEY(entry,item)) ENGINE=InnoDB;
CREATE TABLE world.creature_template_addon (entry INT PRIMARY KEY,path_id INT) ENGINE=InnoDB;
CREATE TABLE characters.ai_agents (agent_id BIGINT PRIMARY KEY,spawn_id BIGINT,map_id INT,control_mode INT) ENGINE=InnoDB;
CREATE TABLE characters.ai_long_term_memories (memory_id BIGINT PRIMARY KEY,agent_id BIGINT,actor_agent_id BIGINT,
target_agent_id BIGINT,actor_spawn_id BIGINT,target_spawn_id BIGINT) ENGINE=InnoDB;
CREATE TABLE characters.ai_agent_groups (group_id BIGINT PRIMARY KEY,territory_map_id INT) ENGINE=InnoDB;
CREATE TABLE characters.ai_agent_group_members (group_id BIGINT,member_agent_id BIGINT,PRIMARY KEY(group_id,member_agent_id)) ENGINE=InnoDB;
CREATE TABLE characters.respawn (type INT,spawnId INT,mapId INT,instanceId INT,PRIMARY KEY(type,spawnId,instanceId)) ENGINE=InnoDB;
CREATE TABLE characters.characters (guid INT PRIMARY KEY,name VARCHAR(50)) ENGINE=InnoDB;
INSERT INTO characters.characters VALUES (12,'Existing player');
INSERT INTO characters.ai_agent_groups VALUES (99,0);
INSERT INTO characters.ai_long_term_memories VALUES (1,901014,901035,0,901035,0),(2,900725,0,0,0,0),(3,33,0,0,0,0);
INSERT INTO characters.respawn VALUES (0,901014,725,0),(1,901014,725,0),(0,12,0,0);
INSERT INTO world.npc_vendor VALUES (11328,123);
INSERT INTO world.creature_template_addon VALUES (11328,70000);
''')
        population=full_population.Population.load()
        values=[]
        for actor in population.settlement.actors:
            values.append(f"({actor.spawn_id},{actor.entry},725,'{actor.label}',{actor.x},{actor.y},{actor.z},{actor.orientation},1,17)")
        for identity in sorted(reset.LEGACY_IDS):
            values.append(f"({identity},1186,725,'legacy',0,0,0,0,0,17)")
        values.append("(12,11328,0,'Unrelated creature',0,0,0,0,1,17)")
        self.sql('INSERT INTO world.creature VALUES '+','.join(values)+';')
        entries=sorted({actor.entry for actor in population.settlement.actors}|{1186})
        self.sql('INSERT INTO world.creature_template VALUES '+','.join(
            f"({entry},'{'Eastvale Peasant' if entry==11328 else 'Source '+str(entry)}',NULL,2.75,1.14286,{entry},'future native value')" for entry in entries)+';')
        ids=sorted(reset.REVIEWED_IDS)
        self.sql('INSERT INTO characters.ai_agents VALUES '+','.join(
            f'({i},{i},725,{int(i in reset.ACTIVE_IDS)})' for i in ids)+',(33,33,0,0);')
        self.sql('INSERT INTO world.ai_spawn_participation_defaults VALUES '+','.join(f'({i},0)' for i in ids)+',(33,0);')
        patch.object(templates,'_query',side_effect=lambda _settings,sql:self.sql(sql)).start()
        patch.object(reset,'_query',side_effect=lambda _settings,sql:self.sql(sql)).start()
        patch.object(reset.manage,'full_preflight',return_value=population).start()
        patch.object(reset.settlement,'ensure_world_offline').start()
        self.addCleanup(patch.stopall)
        self.addCleanup(self.clean)
        specs=tuple(templates.spec_from_document(document(i,name)) for i,name in
                    ((901014,'Šaša'),(901035,'Matěj'),(901036,'Běla')))
        templates.apply(self.settings,specs)

    def sql(self,statement):
        return manage.mysql(self.admin,statement)

    def clean(self):
        self.sql('DROP TABLE IF EXISTS '+','.join('world.`'+table+'`' for table in sorted(self.WORLD_TABLES))+';')
        self.sql('DROP TABLE IF EXISTS '+','.join('characters.`'+table+'`' for table in sorted(self.CHARACTER_TABLES))+';')

    def test_complete_reset_preserves_shared_sources_players_and_other_map(self):
        # Match the real retirement's 32,448 related memories without copying
        # their contents into Python or expanding the transaction into row SQL.
        digits='('+' UNION ALL '.join('SELECT '+str(i)+' n' for i in range(10))+')'
        number='(a.n+10*b.n+100*c.n+1000*d.n+10000*e.n)'
        self.sql('INSERT INTO characters.ai_long_term_memories SELECT 100+'+number+
            ',901014,901035,0,901035,0 FROM '+digits+' a CROSS JOIN '+digits+' b CROSS JOIN '+digits+
            ' c CROSS JOIN '+digits+' d CROSS JOIN '+digits+' e WHERE '+number+'<32446;')
        before=self.sql('SELECT * FROM world.creature_template WHERE entry<9800000 ORDER BY entry;')
        player=self.sql('SELECT * FROM characters.characters;')
        expected=reset.plan(self.settings)
        self.assertEqual(len(expected['spawn_ids']),200)
        self.assertEqual(len(expected['template_entries']),3)
        memory=next(record for record in expected['records'] if record['table']=='ai_long_term_memories')
        self.assertEqual(memory['fingerprint']['count'],32448)
        self.assertEqual(memory['fingerprint']['bytes'],32448*64)
        self.assertNotIn('rows',memory)
        result=reset.apply(self.settings,expected)
        self.assertEqual(result['removed_spawns'],200)
        self.assertEqual(reset.verify_empty(self.settings),dict(spawns=0,agents=0,participation=0))
        self.assertEqual(self.sql('SELECT * FROM world.creature_template ORDER BY entry;'),before)
        self.assertEqual(self.sql('SELECT * FROM world.npc_vendor;'),'11328\t123')
        self.assertEqual(self.sql('SELECT * FROM world.creature_template_addon;'),'11328\t70000')
        self.assertEqual(self.sql('SELECT * FROM characters.characters;'),player)
        self.assertEqual(self.sql('SELECT guid FROM world.creature;'),'12')
        self.assertEqual(self.sql('SELECT agent_id FROM characters.ai_agents;'),'33')
        self.assertEqual(self.sql('SELECT memory_id FROM characters.ai_long_term_memories;'),'3')
        self.assertEqual(self.sql('SELECT group_id FROM characters.ai_agent_groups;'),'99')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.respawn;'),'2')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_character_templates;'),'0')

    def test_stale_plan_refuses_source_or_actor_changes_before_any_delete(self):
        expected=reset.plan(self.settings)
        self.sql('UPDATE world.creature SET curhealth=18 WHERE guid=901014;')
        with self.assertRaisesRegex(reset.ProfileError,'stale'):reset.apply(self.settings,expected)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.creature WHERE map=725;'),'200')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_character_templates;'),'3')
        self.sql('UPDATE world.creature SET curhealth=17 WHERE guid=901014;')
        self.sql("UPDATE world.creature_template SET NewFutureColumn='tampered' WHERE entry=11328;")
        with self.assertRaises(templates.ProfileError):reset.plan(self.settings)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.creature WHERE map=725;'),'200')

    def test_reference_insert_after_replan_is_stopped_by_transaction_guard(self):
        expected=reset.plan(self.settings)
        def inject(settings,sql):
            if 'START TRANSACTION;' in sql:
                self.sql('INSERT INTO world.creature_addon VALUES (901014,999);')
            return self.sql(sql)
        with patch.object(reset,'_query',side_effect=inject):
            with self.assertRaisesRegex(reset.ProfileError,'guard refused'):reset.apply(self.settings,expected)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.creature WHERE map=725;'),'200')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_character_templates;'),'3')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents WHERE map_id=725;'),'200')

    def test_same_count_memory_change_after_replan_is_stopped_by_digest_guard(self):
        expected=reset.plan(self.settings)
        def inject(settings,sql):
            if 'START TRANSACTION;' in sql:
                self.sql('UPDATE characters.ai_long_term_memories SET actor_agent_id=901036 WHERE memory_id=1;')
            return self.sql(sql)
        with patch.object(reset,'_query',side_effect=inject):
            with self.assertRaisesRegex(reset.ProfileError,'guard refused'):reset.apply(self.settings,expected)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.creature WHERE map=725;'),'200')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_character_templates;'),'3')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_long_term_memories;'),'3')


if __name__=='__main__':unittest.main()
