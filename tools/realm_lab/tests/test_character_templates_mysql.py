"""Complete template cloning is exercised only on disposable CI MySQL."""
from dataclasses import replace
import os
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.realm_lab import character_templates as templates, full_population, manage
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_character_templates import document


@unittest.skipUnless(os.getenv('LAB_TEST_MYSQL') == '1', 'Disposable MySQL integration is a CI gate')
class CharacterTemplateMySQLTests(unittest.TestCase):
    def setUp(self):
        self.admin = manage.Settings.load({**environment(), 'LAB_AI_PROFILE':'full-v8',
            'TC_AUTH_DB_HOST':'127.0.0.1', 'TC_AUTH_DB_USER':'root',
            'TC_AUTH_DB_PASSWORD':os.environ['LAB_TEST_MYSQL_ROOT_PASSWORD']})
        self.settings = SimpleNamespace(ai_profile='full-v8')
        tables = {'creature', 'creature_template', templates.LEDGER, *(r.table for r in templates.RESOURCES)}
        self.sql('CREATE DATABASE IF NOT EXISTS world; DROP TABLE IF EXISTS ' + ','.join('world.`'+t+'`' for t in sorted(tables)) + ';')
        self.sql('''CREATE TABLE world.creature_template (
 entry INT UNSIGNED PRIMARY KEY,name VARCHAR(100),subname VARCHAR(100),AIName VARCHAR(64),
 faction SMALLINT,npcflag BIGINT,HealthModifier FLOAT,speed_run FLOAT,lootid INT,
 NewFutureColumn VARCHAR(100),OptionalField INT NULL) ENGINE=InnoDB;
CREATE TABLE world.creature (guid BIGINT PRIMARY KEY,id INT,map INT,StringId VARCHAR(64),
 position_x FLOAT,position_y FLOAT,position_z FLOAT,orientation FLOAT,curhealth INT) ENGINE=InnoDB;
CREATE TABLE world.npc_vendor (entry INT,item INT,maxcount INT,incrtime INT,PRIMARY KEY(entry,item)) ENGINE=InnoDB;
CREATE TABLE world.creature_template_locale (entry INT,locale VARCHAR(4),Name TEXT,Title TEXT,PRIMARY KEY(entry,locale)) ENGINE=InnoDB;
CREATE TABLE world.creature_equip_template (CreatureID INT,ID INT,ItemID1 INT,ItemID2 INT NULL,PRIMARY KEY(CreatureID,ID)) ENGINE=InnoDB;
CREATE TABLE world.creature_default_trainer (CreatureId INT PRIMARY KEY,TrainerId INT) ENGINE=InnoDB;
CREATE TABLE world.creature_queststarter (id INT,quest INT,PRIMARY KEY(id,quest)) ENGINE=InnoDB;
CREATE TABLE world.creature_template_addon (entry INT PRIMARY KEY,path_id INT,auras TEXT) ENGINE=InnoDB;
CREATE TABLE world.conditions (SourceTypeOrReferenceId INT,SourceGroup INT,SourceEntry INT,SourceId INT,ElseGroup INT,
 ConditionTypeOrReference INT,ConditionValue1 INT,Comment TEXT,
 PRIMARY KEY(SourceTypeOrReferenceId,SourceGroup,SourceEntry,SourceId,ElseGroup,ConditionTypeOrReference)) ENGINE=InnoDB;
INSERT INTO world.creature_template VALUES (11328,'Eastvale Peasant',NULL,'',12,129,2.75,1.14286,11328,'future inherited field',NULL);
INSERT INTO world.npc_vendor VALUES (11328,123,2,60);
INSERT INTO world.creature_template_locale VALUES (11328,'deDE','Bauer','alt'),(11328,'frFR','Paysan',NULL);
INSERT INTO world.creature_equip_template VALUES (11328,1,17,NULL);
INSERT INTO world.creature_default_trainer VALUES (11328,333);
INSERT INTO world.creature_queststarter VALUES (11328,456);
INSERT INTO world.creature_template_addon VALUES (11328,789,'1 2 3');
INSERT INTO world.conditions VALUES (23,11328,123,0,0,1,7,'vendor condition'),(16,0,11328,0,0,1,8,'vehicle condition');
''')
        population = full_population.Population.load()
        for identity in (901014,901035,901036):
            actor = next(a for a in population.settlement.actors if a.spawn_id == identity)
            self.assertEqual(actor.entry,11328)
            self.sql(f"INSERT INTO world.creature VALUES ({identity},11328,725,'{actor.label}',{actor.x},{actor.y},{actor.z},{actor.orientation},17);")
        self.query_patcher = patch.object(templates, '_query', side_effect=lambda _settings, sql:self.sql(sql))
        self.query_patcher.start(); self.addCleanup(self.query_patcher.stop)
        self.offline = patch.object(templates.settlement, 'ensure_world_offline')
        self.offline.start(); self.addCleanup(self.offline.stop)
        self.addCleanup(lambda:self.sql('DROP TABLE IF EXISTS world.'+templates.LEDGER+';'))

    def sql(self, statement): return manage.mysql(self.admin,statement)

    def spec(self, identity=901014, name='Šaša', title='Obyvatelka'):
        return templates.spec_from_document(document(identity,name,title))

    def test_complete_owned_clones_keep_shared_source_all_columns_and_child_services(self):
        schema=templates._schema(self.settings)
        before=templates._bundle(self.settings,schema,11328)
        original_spawns=self.sql('SELECT * FROM world.creature ORDER BY guid;')
        sasa, matej = self.spec(),self.spec(901035,'Matěj','Hospodář')
        for _ in range(2):
            self.assertEqual(templates.apply(self.settings,(sasa,matej)),(sasa,matej))
        self.assertEqual(templates._bundle(self.settings,templates._schema(self.settings),11328),before)
        self.assertEqual(self.sql('SELECT name,subname,NewFutureColumn,HealthModifier,OptionalField IS NULL FROM world.creature_template WHERE entry=9800014;'),
                         'Šaša\tObyvatelka\tfuture inherited field\t2.75\t1')
        self.assertEqual(self.sql('SELECT DISTINCT Name,Title FROM world.creature_template_locale WHERE entry=9800014;'),'Šaša\tObyvatelka')
        self.assertEqual(self.sql('SELECT item,maxcount,incrtime FROM world.npc_vendor WHERE entry=9800014;'),'123\t2\t60')
        self.assertEqual(self.sql('SELECT TrainerId FROM world.creature_default_trainer WHERE CreatureId=9800014;'),'333')
        self.assertEqual(self.sql('SELECT quest FROM world.creature_queststarter WHERE id=9800014;'),'456')
        self.assertEqual(self.sql('SELECT ConditionValue1 FROM world.conditions WHERE SourceGroup=9800014 AND SourceTypeOrReferenceId=23;'),'7')
        self.assertEqual(self.sql('SELECT ConditionValue1 FROM world.conditions WHERE SourceEntry=9800014 AND SourceTypeOrReferenceId=16;'),'8')
        self.assertEqual(self.sql('SELECT id FROM world.creature WHERE guid=901036;'),'11328')
        renamed=replace(sasa,name='Šaša Nová',title='Nový titul')
        templates.restore(self.settings)
        self.assertEqual(self.sql('SELECT * FROM world.creature ORDER BY guid;'),original_spawns)
        self.assertEqual(templates.apply(self.settings,(renamed,)),(renamed,matej))
        self.assertEqual(self.sql('SELECT name FROM world.creature_template WHERE entry=9800035;'),'Matěj')
        self.assertEqual(self.sql('SELECT name FROM world.creature_template WHERE entry=11328;'),'Eastvale Peasant')
        with patch.object(templates,'load_descriptors',side_effect=AssertionError('runtime must not load files')):
            self.assertEqual(templates.verify(self.settings),(renamed,matej))
        templates.restore(self.settings)
        with self.assertRaises(templates.ProfileError):templates.verify(self.settings)
        self.assertEqual(templates.verify(self.settings,allow_restored=True),(renamed,matej))

    def test_collisions_and_source_target_child_or_spawn_drift_prevent_adoption(self):
        sasa=self.spec()
        self.sql('INSERT INTO world.creature_template SELECT 9800014,name,subname,AIName,faction,npcflag,HealthModifier,speed_run,lootid,NewFutureColumn,OptionalField FROM world.creature_template WHERE entry=11328;')
        with self.assertRaises(templates.ProfileError):templates.apply(self.settings,(sasa,))
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_character_templates;'),'0')
        self.assertEqual(self.sql('SELECT id FROM world.creature WHERE guid=901014;'),'11328')
        self.sql('DELETE FROM world.creature_template WHERE entry=9800014;')
        templates.apply(self.settings,(sasa,))
        for corrupt, repair in (
            ("UPDATE world.creature_template SET NewFutureColumn='drift' WHERE entry=9800014;","UPDATE world.creature_template SET NewFutureColumn='future inherited field' WHERE entry=9800014;"),
            ('UPDATE world.npc_vendor SET maxcount=4 WHERE entry=9800014;','UPDATE world.npc_vendor SET maxcount=2 WHERE entry=9800014;'),
            ("UPDATE world.creature_template SET name='changed source' WHERE entry=11328;","UPDATE world.creature_template SET name='Eastvale Peasant' WHERE entry=11328;"),
            ('UPDATE world.creature SET id=9800014 WHERE guid=901036;','UPDATE world.creature SET id=11328 WHERE guid=901036;')):
            with self.subTest(corrupt=corrupt):
                self.sql(corrupt)
                with self.assertRaises(templates.ProfileError):templates.verify(self.settings)
                with self.assertRaises(templates.ProfileError):templates.restore(self.settings)
                self.sql(repair)
        self.assertEqual(templates.verify(self.settings),(sasa,))


if __name__ == '__main__':unittest.main()
