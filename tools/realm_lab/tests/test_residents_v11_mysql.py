"""V11 upgrades existing household spawns without rewriting their homes."""
from dataclasses import replace
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, residents_population as old, residents_v11 as current, settlement_population, single_return
from tools.realm_lab.tests import test_residents_population_mysql as old_mysql
from tools.realm_lab.tests.test_residents_v11 import fixture


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL")=="1","Disposable MySQL integration is a CI gate")
class V11ResidentsMySQLTests(unittest.TestCase):
    def setUp(self):
        old_mysql.ResidentPopulationMySQLTests.setUp(self)
        self.baseline=self.population
        extra=tempfile.TemporaryDirectory();self.addCleanup(extra.cleanup)
        metadata,geometry,_,_,baseline=fixture(Path(extra.name))
        patcher=patch.object(old.Population,"load",return_value=baseline);patcher.start();self.addCleanup(patcher.stop)
        self.population=current.Population.load(metadata,geometry)
        self.admin=replace(self.admin,ai_profile=current.PROFILE)
        fields=settlement_population.SMART_FIELDS
        columns=",".join('`'+f+'` '+('TEXT' if f=='comment' else 'DOUBLE' if f in ('target_x','target_y','target_z','target_o') else 'BIGINT') for f in fields)
        self.sql(f"""INSERT INTO world.creature_template VALUES (38,'SmartAI','',1,0,0,0,2048,2300,3,4,0);
DROP TABLE world.smart_scripts;
CREATE TABLE world.smart_scripts ({columns});
INSERT INTO world.creature_template_addon VALUES (38,0,0,0);
DROP TABLE IF EXISTS world.ai_world_faction_entry_defaults;
CREATE TABLE world.ai_world_faction_entry_defaults (creature_entry INT PRIMARY KEY,world_faction_id INT);
INSERT INTO world.ai_world_faction_entry_defaults VALUES (38,2),(11328,1);
""")
        literal=lambda value:"'"+value.replace("'","''")+"'" if isinstance(value,str) else str(value)
        script=current.DEFIAS_SMART_SCRIPTS[0]
        self.sql("INSERT INTO world.smart_scripts VALUES ("+",".join(literal(script[f]) for f in fields)+");")

    def sql(self,statement):return manage.mysql(self.admin,statement)
    def snapshot(self):return single_return.parse_snapshot(self.sql(current.snapshot_sql(self.population)))

    def test_existing_22_homes_survive_45_actor_bootstrap_and_repeated_deploy(self):
        self.sql(old.bootstrap_sql(self.baseline))
        before=self.sql('SELECT * FROM world.creature WHERE guid BETWEEN 902000 AND 902021 ORDER BY guid;')
        for _ in range(2):
            current.validate_snapshot(self.snapshot(),self.population,bootstrap=True)
            self.sql(current.bootstrap_sql(self.population))
            current.validate_snapshot(self.snapshot(),self.population)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.creature WHERE map=725;'),'45')
        self.assertEqual(self.sql('SELECT * FROM world.creature WHERE guid BETWEEN 902000 AND 902021 ORDER BY guid;'),before)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.creature WHERE id=38 AND guid BETWEEN 902035 AND 902044;'),'10')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM world.ai_world_faction_entry_defaults WHERE world_faction_id=2 AND creature_entry BETWEEN 9810000 AND 9810044;'),'10')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM characters.ai_agents;'),'0')

    def test_foreign_camp_faction_or_changed_combat_script_refuses_adoption(self):
        self.sql(current.bootstrap_sql(self.population))
        self.sql('UPDATE world.ai_world_faction_entry_defaults SET world_faction_id=1 WHERE creature_entry=9810035;')
        with self.assertRaises(current.ProfileError):current.validate_snapshot(self.snapshot(),self.population,bootstrap=True)
        self.sql('UPDATE world.ai_world_faction_entry_defaults SET world_faction_id=2 WHERE creature_entry=9810035;')
        self.sql('UPDATE world.smart_scripts SET action_type=46 WHERE entryorguid=38;')
        with self.assertRaises(current.ProfileError):current.validate_snapshot(self.snapshot(),self.population,bootstrap=True)


if __name__=='__main__':unittest.main()
