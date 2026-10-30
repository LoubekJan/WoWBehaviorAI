"""Full ownership transactions run only against the disposable CI MySQL."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import defias_population as defias, full_population as full, settlement_population as settlement
from tools.realm_lab.tests import test_defias_population_mysql as defias_mysql
from tools.realm_lab.tests.test_full_population import write_full_metadata


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class FullPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.fixture = defias_mysql.DefiasPopulationMySQLTests(
            "test_repeat_extension_preserves_existing_forty_all_homes_templates_memory_economy_and_legacy")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.prepare()
        self.sql(defias.activation_sql(self.fixture.population))
        defias.validate_snapshot(self.fixture.rows(), self.fixture.population)
        leaf = self.fixture
        while hasattr(leaf, "fixture"): leaf = leaf.fixture
        metadata = Path(leaf.tmp.name) / "metadata"
        sha = write_full_metadata(metadata, self.fixture.population)
        patcher = patch.object(full, "BASE_DEFIAS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = full.Population.load(metadata)

    def sql(self, statement): return self.fixture.sql(statement)
    def rows(self): return full.parse_snapshot(self.sql(full.snapshot_sql(self.population)))

    def prepare(self):
        full.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(full.bootstrap_sql(self.population))
        full.validate_snapshot(self.rows(), self.population, control=None)

    def test_repeat_full_activation_preserves_previous_fifty_five_and_all_native_service_contracts(self):
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31 WHERE agent_id IN (901016,901035,901055);
INSERT INTO characters.ai_long_term_memories VALUES (1,901035,0.875,7),(2,901055,0.625,3);""")
        ids = ",".join(str(identity) for identity in sorted(full.RECRUIT_IDS))
        query = ("SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,"
                 "work_map_id,work_x,work_y,work_z,work_o,world_faction_id,money,food,resource "
                 "FROM characters.ai_agents")
        queries = (query + f" WHERE agent_id NOT IN ({ids}) ORDER BY agent_id;",
            "SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;",
            "SELECT * FROM world.creature_template ORDER BY entry;", "SELECT * FROM world.creature ORDER BY guid;",
            "SELECT * FROM world.ai_agent_type_entry_defaults ORDER BY creature_entry;",
            "SELECT * FROM world.smart_scripts ORDER BY entryorguid,id;", "SHOW TABLES FROM auth;")
        before = {sql: self.sql(sql) for sql in queries}
        legacy = settlement.legacy_snapshot_digest(self.rows())
        self.prepare()
        self.sql("UPDATE characters.ai_agents SET money=19,food=29,resource=37 WHERE agent_id=901014;")
        prepared = self.sql(query + " ORDER BY agent_id;")
        for _ in range(2):
            self.sql(full.bootstrap_sql(self.population))
            full.validate_snapshot(self.rows(), self.population, control=None)
            self.sql(full.activation_sql(self.population))
            full.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "100")
        self.assertEqual(self.sql("SELECT agent_type,world_faction_id FROM characters.ai_agents WHERE agent_id=901014;"), "0\t1")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE agent_type=2 AND agent_id BETWEEN 901000 AND 901099;"), "7")
        self.assertEqual(self.sql(query + " ORDER BY agent_id;"), prepared)
        for sql in queries:
            with self.subTest(sql=sql): self.assertEqual(self.sql(sql), before[sql])
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), legacy)
        self.sql(full.deactivation_sql(self.population))
        full.validate_snapshot(self.rows(), self.population, control=0)

    def test_activation_refuses_new_or_baseline_drift_without_changing_existing_fifty_five(self):
        self.prepare()
        resident = next(a for a in self.population.recruits if a.spawn_id == 901014)
        mutations = (
            ("UPDATE characters.ai_agents SET agent_type=1 WHERE agent_id=901014;", "UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901014;"),
            ("UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901008;", "UPDATE characters.ai_agents SET agent_type=2 WHERE agent_id=901008;"),
            ("UPDATE characters.ai_agents SET world_faction_id=1 WHERE agent_id=901066;", "UPDATE characters.ai_agents SET world_faction_id=4 WHERE agent_id=901066;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901014;", f"UPDATE characters.ai_agents SET home_x={resident.x} WHERE agent_id=901014;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901092;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901092;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901014;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901014;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901014);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901014;"),
            ("UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901055;", "UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901055;"))
        for mutate, restore in mutations:
            with self.subTest(mutation=mutate):
                self.sql(mutate)
                with self.assertRaises(full.ProfileError): full.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(full.activation_sql(self.population))
                self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({','.join(str(i) for i in sorted(full.RECRUIT_IDS))});"), "0")
                self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.defias_population.spawn_ids});"), "55")
                self.sql(restore)
        self.sql(full.activation_sql(self.population))
        full.validate_snapshot(self.rows(), self.population)


if __name__ == "__main__": unittest.main()
