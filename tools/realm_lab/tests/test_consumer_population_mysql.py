"""Execute the ordinary-consumer extension against disposable CI MySQL only."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import consumer_population as consumers, settlement_population as settlement
from tools.realm_lab import supply_population as supply, wildlife_population as wildlife
from tools.realm_lab.tests import test_supply_population_mysql as supply_mysql
from tools.realm_lab.tests.test_consumer_population import write_consumer_metadata


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class ConsumerPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.fixture = supply_mysql.SupplyPopulationMySQLTests(
            "test_repeated_extension_preserves_wildlife_memory_economy_native_templates_and_legacy")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.prepare()
        self.sql(supply.activation_sql(self.fixture.population))
        supply.validate_snapshot(self.fixture.rows(), self.fixture.population)
        metadata = Path(self.fixture.fixture.tmp.name) / "metadata"
        sha = write_consumer_metadata(metadata)
        patcher = patch.object(consumers, "BASE_SUPPLY_SHA256", sha)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.population = consumers.Population.load(metadata)
        self.before_types = self.sql("SELECT * FROM world.ai_agent_type_entry_defaults ORDER BY creature_entry;")

    def sql(self, statement):
        return self.fixture.sql(statement)

    def rows(self):
        return consumers.parse_snapshot(self.sql(consumers.snapshot_sql(self.population)))

    def prepare(self):
        consumers.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(consumers.bootstrap_sql(self.population))
        consumers.validate_snapshot(self.rows(), self.population, control=None)

    def assert_new_consumers_inactive(self):
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN (901035,901036,901063);"), "0")
        self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.supply.spawn_ids});"), "23")

    def test_repeated_setup_preserves_ordinary_types_identity_economy_memory_templates_and_legacy(self):
        self.prepare()
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31 WHERE agent_id IN (901034,901035);
INSERT INTO characters.ai_long_term_memories VALUES (1,901034,0.875,7),(2,901035,0.625,3);""")
        identity_query = """SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,
 work_map_id,work_x,work_y,work_z,work_o,world_faction_id FROM characters.ai_agents ORDER BY agent_id;"""
        identity = self.sql(identity_query)
        persistent_query = "SELECT agent_id,money,food,resource FROM characters.ai_agents WHERE agent_id IN (901034,901035) ORDER BY agent_id;"
        persistent, memory = self.sql(persistent_query), self.sql("SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;")
        templates = self.sql("SELECT * FROM world.creature_template ORDER BY entry;")
        for _ in range(2):
            self.sql(wildlife.bootstrap_sql(self.population.wildlife))
            self.sql(supply.bootstrap_sql(self.population.supply))
            self.sql(consumers.bootstrap_sql(self.population))
            consumers.validate_snapshot(self.rows(), self.population, control=None)
            self.sql(consumers.activation_sql(self.population))
            consumers.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "26")
        self.assertEqual(self.sql("SELECT agent_type FROM characters.ai_agents WHERE agent_id IN (901035,901036,901063) ORDER BY agent_id;"), "0\n0\n5")
        self.assertEqual(self.sql(identity_query), identity)
        self.assertEqual(self.sql(persistent_query), persistent)
        self.assertEqual(self.sql("SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;"), memory)
        self.assertEqual(self.sql("SELECT * FROM world.ai_agent_type_entry_defaults ORDER BY creature_entry;"), self.before_types)
        self.assertEqual(self.sql("SELECT * FROM world.creature_template ORDER BY entry;"), templates)
        self.assertEqual(self.sql("SELECT * FROM world.creature ORDER BY guid;"), self.fixture.fixture.before_native)
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), self.fixture.fixture.legacy_readback_sha256)
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.fixture.fixture.auth_tables)

    def test_consumer_drift_cannot_activate_or_disturb_existing_twenty_three(self):
        self.prepare()
        worker = next(actor for actor in self.population.consumers if actor.spawn_id == 901035)
        template = next(t for t in self.population.settlement.templates if t["entry"] == 97)
        mutations = (
            ("UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901035;", "UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901035;"),
            ("UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901063;", "UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901063;"),
            ("UPDATE characters.ai_agents SET world_faction_id=3 WHERE agent_id=901035;", "UPDATE characters.ai_agents SET world_faction_id=1 WHERE agent_id=901035;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901035;", f"UPDATE characters.ai_agents SET home_x={worker.x} WHERE agent_id=901035;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901035;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901035;"),
            ("UPDATE world.creature_template SET flags_extra=1 WHERE entry=97;", f"UPDATE world.creature_template SET flags_extra={template['flags_extra']} WHERE entry=97;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901036;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901036;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901063);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901063;"),
            ("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);", "DELETE FROM characters.ai_agents WHERE agent_id=999;"),
            ("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (901037,0,725,901037,0);", "DELETE FROM characters.ai_agents WHERE agent_id=901037;"),
        )
        for mutate, restore in mutations:
            with self.subTest(mutation=mutate):
                self.sql(mutate)
                with self.assertRaises(consumers.ProfileError):
                    consumers.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(consumers.activation_sql(self.population))
                self.assert_new_consumers_inactive()
                self.sql(restore)
        self.sql(consumers.activation_sql(self.population))
        consumers.validate_snapshot(self.rows(), self.population)


if __name__ == "__main__":
    unittest.main()
