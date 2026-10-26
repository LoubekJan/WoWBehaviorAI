"""Execute the new ownership transactions against disposable CI MySQL only."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import settlement_population as settlement, supply_population as supply, wildlife_population as wildlife
from tools.realm_lab.tests import test_wildlife_population_mysql as wildlife_mysql
from tools.realm_lab.tests.test_supply_population import write_supply_metadata


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class SupplyPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        # Reuse the real native/legacy schema and its SQL-readback calibration;
        # do not seed a different representation of the protected old world.
        self.fixture = wildlife_mysql.WildlifePopulationMySQLTests(
            "test_bootstrap_and_activation_execute_twice_without_resetting_native_or_persistent_state")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        metadata = Path(self.fixture.tmp.name) / "metadata"
        sha = write_supply_metadata(metadata)
        patcher = patch.object(supply, "BASE_WILDLIFE_SHA256", sha)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.population = supply.Population.load(metadata)
        self.sql(wildlife.bootstrap_sql(self.population.wildlife))
        self.sql(wildlife.activation_sql(self.population.wildlife))
        wildlife.validate_snapshot(self.fixture.rows(), self.population.wildlife)

    def sql(self, statement):
        return self.fixture.sql(statement)

    def rows(self):
        return supply.parse_snapshot(self.sql(supply.snapshot_sql(self.population)))

    def prepare(self):
        supply.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(supply.bootstrap_sql(self.population))
        supply.validate_snapshot(self.rows(), self.population, control=None)

    def assert_new_hunters_inactive(self):
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN (901034,901061,901062);"), "0")
        self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.wildlife.spawn_ids});"), "20")

    def test_repeated_extension_preserves_wildlife_memory_economy_native_templates_and_legacy(self):
        self.prepare()
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31 WHERE agent_id IN (901034,901072);
INSERT INTO characters.ai_long_term_memories VALUES (1,901034,0.875,7),(2,901072,0.625,3);""")
        persistent = self.sql("SELECT agent_id,money,food,resource FROM characters.ai_agents WHERE agent_id IN (901034,901072) ORDER BY agent_id;")
        memory = self.sql("SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;")
        templates = self.sql("SELECT * FROM world.creature_template ORDER BY entry;")
        for _ in range(2):
            self.sql(wildlife.bootstrap_sql(self.population.wildlife))
            self.sql(supply.bootstrap_sql(self.population))
            supply.validate_snapshot(self.rows(), self.population, control=None)
            self.sql(supply.activation_sql(self.population))
            supply.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "23")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM world.ai_agent_type_entry_defaults WHERE creature_entry IN (478,11328);"), "0")
        self.assertEqual(self.sql("SELECT * FROM world.creature ORDER BY guid;"), self.fixture.before_native)
        self.assertEqual(self.sql("SELECT * FROM world.creature_template ORDER BY entry;"), templates)
        self.assertEqual(self.sql("SELECT * FROM characters.ai_agents WHERE agent_id BETWEEN 900725 AND 900824 ORDER BY agent_id;"), self.fixture.before_legacy)
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), self.fixture.legacy_readback_sha256)
        self.assertEqual(self.sql("SELECT agent_id,money,food,resource FROM characters.ai_agents WHERE agent_id IN (901034,901072) ORDER BY agent_id;"), persistent)
        self.assertEqual(self.sql("SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;"), memory)
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.fixture.auth_tables)

    def test_sql_activation_refuses_hunter_drift_and_outside_ownership_without_disturbing_twenty_animals(self):
        self.prepare()
        human = next(actor for actor in self.population.hunters if actor.spawn_id == 901034)
        template = next(t for t in self.population.settlement.templates if t["entry"] == 11328)
        mutations = (
            ("UPDATE characters.ai_agents SET agent_type=3 WHERE agent_id=901034;", "UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901034;"),
            ("UPDATE characters.ai_agents SET world_faction_id=2 WHERE agent_id=901034;", "UPDATE characters.ai_agents SET world_faction_id=1 WHERE agent_id=901034;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901034;", f"UPDATE characters.ai_agents SET home_x={human.x} WHERE agent_id=901034;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901034;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901034;"),
            ("UPDATE world.creature_template SET flags_extra=0 WHERE entry=11328;", f"UPDATE world.creature_template SET flags_extra={template['flags_extra']} WHERE entry=11328;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901061;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901061;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901062);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901062;"),
            ("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);", "DELETE FROM characters.ai_agents WHERE agent_id=999;"),
        )
        for mutate, restore in mutations:
            with self.subTest(mutation=mutate):
                self.sql(mutate)
                with self.assertRaises(supply.ProfileError):
                    supply.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(supply.activation_sql(self.population))
                self.assert_new_hunters_inactive()
                self.sql(restore)
        self.sql(supply.activation_sql(self.population))
        supply.validate_snapshot(self.rows(), self.population)


if __name__ == "__main__":
    unittest.main()
