"""Run guarded raid activation against disposable CI MySQL only."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import raid_population as raids, consumer_population as consumers, settlement_population as settlement
from tools.realm_lab import supply_population as supply, wildlife_population as wildlife
from tools.realm_lab.tests import test_consumer_population_mysql as consumer_mysql
from tools.realm_lab.tests.test_raid_population import write_raid_metadata


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class RaidPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.fixture = consumer_mysql.ConsumerPopulationMySQLTests(
            "test_repeated_setup_preserves_ordinary_types_identity_economy_memory_templates_and_legacy")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.prepare()
        self.sql(consumers.activation_sql(self.fixture.population))
        consumers.validate_snapshot(self.fixture.rows(), self.fixture.population)
        metadata = Path(self.fixture.fixture.fixture.tmp.name) / "metadata"
        sha = write_raid_metadata(metadata)
        patcher = patch.object(raids, "BASE_CONSUMERS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = raids.Population.load(metadata)

    def sql(self, statement):
        return self.fixture.sql(statement)

    def rows(self):
        return raids.parse_snapshot(self.sql(raids.snapshot_sql(self.population)))

    def prepare(self):
        raids.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(raids.bootstrap_sql(self.population))
        raids.validate_snapshot(self.rows(), self.population, control=None)

    def test_repeated_raid_bootstrap_preserves_all_existing_identities_memory_economy_native_geometry_and_templates(self):
        self.prepare()
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31 WHERE agent_id IN (901034,901035,901039,901055);
INSERT INTO characters.ai_long_term_memories VALUES (1,901034,0.875,7),(2,901039,0.625,3);""")
        identity_query = """SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,
 work_map_id,work_x,work_y,work_z,work_o,world_faction_id FROM characters.ai_agents ORDER BY agent_id;"""
        persistent_query = "SELECT agent_id,money,food,resource FROM characters.ai_agents ORDER BY agent_id;"
        memory_query = "SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;"
        queries = (identity_query, persistent_query, memory_query,
                   "SELECT * FROM world.creature_template ORDER BY entry;",
                   "SELECT * FROM world.creature ORDER BY guid;",
                   "SELECT * FROM world.ai_agent_type_entry_defaults ORDER BY creature_entry;",
                   "SHOW TABLES FROM auth;")
        before = {query: self.sql(query) for query in queries}
        legacy = settlement.legacy_snapshot_digest(self.rows())
        for _ in range(2):
            self.sql(wildlife.bootstrap_sql(self.population.wildlife))
            self.sql(supply.bootstrap_sql(self.population.supply))
            self.sql(consumers.bootstrap_sql(self.population.consumers))
            self.sql(raids.bootstrap_sql(self.population))
            raids.validate_snapshot(self.rows(), self.population, control=None)
            self.sql(raids.activation_sql(self.population))
            raids.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "28")
        self.assertEqual(self.sql("SELECT agent_type,world_faction_id FROM characters.ai_agents WHERE agent_id IN (901039,901055) ORDER BY agent_id;"), "1\t1\n5\t2")
        for query in queries:
            with self.subTest(query=query): self.assertEqual(self.sql(query), before[query])
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), legacy)

    def test_sql_activation_refuses_new_actor_or_old_baseline_drift_without_disturbing_twenty_six(self):
        self.prepare()
        guard = next(a for a in self.population.recruits if a.spawn_id == 901039)
        template = next(t for t in self.population.settlement.templates if t["entry"] == 116)
        mutations = (
            ("UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901039;", "UPDATE characters.ai_agents SET agent_type=1 WHERE agent_id=901039;"),
            ("UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901055;", "UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901055;"),
            ("UPDATE characters.ai_agents SET world_faction_id=3 WHERE agent_id=901055;", "UPDATE characters.ai_agents SET world_faction_id=2 WHERE agent_id=901055;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901039;", f"UPDATE characters.ai_agents SET home_x={guard.x} WHERE agent_id=901039;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901055;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901055;"),
            ("UPDATE world.creature_template SET flags_extra=1 WHERE entry=116;", f"UPDATE world.creature_template SET flags_extra={template['flags_extra']} WHERE entry=116;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901039;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901039;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901055);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901055;"),
            ("UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901035;", "UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901035;"),
            ("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);", "DELETE FROM characters.ai_agents WHERE agent_id=999;"),
            ("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (901037,0,725,901037,0);", "DELETE FROM characters.ai_agents WHERE agent_id=901037;"),
        )
        for mutate, restore in mutations:
            with self.subTest(mutation=mutate):
                self.sql(mutate)
                with self.assertRaises(raids.ProfileError): raids.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(raids.activation_sql(self.population))
                self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN (901039,901055);"), "0")
                self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.consumers.spawn_ids});"), "26")
                self.sql(restore)
        self.sql(raids.activation_sql(self.population))
        raids.validate_snapshot(self.rows(), self.population)


if __name__ == "__main__": unittest.main()
