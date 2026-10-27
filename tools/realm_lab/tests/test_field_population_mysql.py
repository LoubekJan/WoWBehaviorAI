"""Execute forty-actor field activation against disposable CI MySQL only."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import field_population as fields, raid_population as raids, settlement_population as settlement
from tools.realm_lab.tests import test_raid_population_mysql as raid_mysql
from tools.realm_lab.tests.test_field_population import write_field_metadata


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class FieldPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.fixture = raid_mysql.RaidPopulationMySQLTests(
            "test_repeated_raid_bootstrap_preserves_all_existing_identities_memory_economy_native_geometry_and_templates")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.prepare()
        self.sql(raids.activation_sql(self.fixture.population))
        raids.validate_snapshot(self.fixture.rows(), self.fixture.population)
        metadata = Path(self.fixture.fixture.fixture.fixture.tmp.name) / "metadata"
        sha = write_field_metadata(metadata)
        patcher = patch.object(fields, "BASE_RAIDS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = fields.Population.load(metadata)

    def sql(self, statement): return self.fixture.sql(statement)
    def rows(self): return fields.parse_snapshot(self.sql(fields.snapshot_sql(self.population)))

    def prepare(self):
        fields.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(fields.bootstrap_sql(self.population))
        fields.validate_snapshot(self.rows(), self.population, control=None)

    def test_repeated_field_extension_keeps_all_identity_economy_memory_template_and_legacy_state(self):
        self.prepare()
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31 WHERE agent_id IN (901016,901039,901055);
INSERT INTO characters.ai_long_term_memories VALUES (1,901016,0.875,7),(2,901039,0.625,3);""")
        queries = (
            "SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,work_map_id,work_x,work_y,work_z,work_o,world_faction_id,money,food,resource FROM characters.ai_agents ORDER BY agent_id;",
            "SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;",
            "SELECT * FROM world.creature_template ORDER BY entry;",
            "SELECT * FROM world.creature ORDER BY guid;",
            "SELECT * FROM world.ai_agent_type_entry_defaults ORDER BY creature_entry;", "SHOW TABLES FROM auth;")
        before = {query: self.sql(query) for query in queries}
        legacy = settlement.legacy_snapshot_digest(self.rows())
        for _ in range(2):
            self.sql(fields.bootstrap_sql(self.population))
            fields.validate_snapshot(self.rows(), self.population, control=None)
            self.sql(fields.activation_sql(self.population))
            fields.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "40")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE agent_id BETWEEN 901016 AND 901019 AND agent_type=0 AND world_faction_id=1;"), "4")
        for query in queries:
            with self.subTest(query=query): self.assertEqual(self.sql(query), before[query])
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), legacy)
        self.sql(fields.deactivation_sql(self.population))
        fields.validate_snapshot(self.rows(), self.population, control=0)

    def test_field_activation_rechecks_every_worker_and_original_raid_guard(self):
        self.prepare()
        farmer = self.population.recruits[0]
        mutations = (
            ("UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901016;", "UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901016;"),
            ("UPDATE characters.ai_agents SET world_faction_id=2 WHERE agent_id=901016;", "UPDATE characters.ai_agents SET world_faction_id=1 WHERE agent_id=901016;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901016;", f"UPDATE characters.ai_agents SET home_x={farmer.x} WHERE agent_id=901016;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901016;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901016;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901016;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901016;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901016);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901016;"),
            ("UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901055;", "UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901055;"),
            ("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);", "DELETE FROM characters.ai_agents WHERE agent_id=999;"))
        for mutate, restore in mutations:
            with self.subTest(mutation=mutate):
                self.sql(mutate)
                with self.assertRaises(fields.ProfileError): fields.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(fields.activation_sql(self.population))
                self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN (901016,901017,901018,901019,901022,901023,901024,901025,901028,901029,901030,901031);"), "0")
                self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.raids.spawn_ids});"), "28")
                self.sql(restore)
        self.sql(fields.activation_sql(self.population))
        fields.validate_snapshot(self.rows(), self.population)


if __name__ == "__main__": unittest.main()
