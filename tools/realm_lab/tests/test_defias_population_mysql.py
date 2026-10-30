"""Execute the whole-camp activation against disposable CI MySQL only."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.realm_lab import defias_population as defias, field_population as fields, settlement_population as settlement
from tools.realm_lab.tests import test_field_population_mysql as field_mysql
from tools.realm_lab.tests.test_defias_population import write_defias_metadata


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class DefiasPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.fixture = field_mysql.FieldPopulationMySQLTests(
            "test_repeated_field_extension_keeps_all_identity_economy_memory_template_and_legacy_state")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.fixture.prepare()
        self.sql(fields.activation_sql(self.fixture.population))
        fields.validate_snapshot(self.fixture.rows(), self.fixture.population)
        metadata = Path(self.fixture.fixture.fixture.fixture.fixture.tmp.name) / "metadata"
        sha = write_defias_metadata(metadata, self.fixture.population)
        patcher = patch.object(defias, "BASE_FIELDS_SHA256", sha)
        patcher.start(); self.addCleanup(patcher.stop)
        self.population = defias.Population.load(metadata)

    def sql(self, statement): return self.fixture.sql(statement)
    def rows(self): return defias.parse_snapshot(self.sql(defias.snapshot_sql(self.population)))

    def prepare(self):
        defias.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(defias.bootstrap_sql(self.population))
        defias.validate_snapshot(self.rows(), self.population, control=None)

    def test_repeat_extension_preserves_existing_forty_all_homes_templates_memory_economy_and_legacy(self):
        queries = (
            "SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,work_map_id,work_x,work_y,work_z,work_o,world_faction_id,money,food,resource FROM characters.ai_agents WHERE agent_id NOT BETWEEN 901044 AND 901054 AND agent_id NOT BETWEEN 901056 AND 901059 ORDER BY agent_id;",
            "SELECT * FROM characters.ai_long_term_memories ORDER BY memory_id;",
            "SELECT * FROM world.creature_template ORDER BY entry;",
            "SELECT * FROM world.creature ORDER BY guid;",
            "SELECT * FROM world.ai_agent_type_entry_defaults ORDER BY creature_entry;", "SHOW TABLES FROM auth;")
        self.sql("""UPDATE characters.ai_agents SET money=17,food=23,resource=31 WHERE agent_id IN (901016,901039,901055);
INSERT INTO characters.ai_long_term_memories VALUES (1,901016,0.875,7),(2,901055,0.625,3);""")
        before = {query: self.sql(query) for query in queries}
        legacy = settlement.legacy_snapshot_digest(self.rows())
        self.prepare()
        self.sql("UPDATE characters.ai_agents SET money=19,food=29,resource=37 WHERE agent_id=901050;")
        all_identity = "SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,work_map_id,work_x,work_y,work_z,work_o,world_faction_id,money,food,resource FROM characters.ai_agents ORDER BY agent_id;"
        prepared = self.sql(all_identity)
        for _ in range(2):
            self.sql(defias.bootstrap_sql(self.population))
            defias.validate_snapshot(self.rows(), self.population, control=None)
            self.sql(defias.activation_sql(self.population))
            defias.validate_snapshot(self.rows(), self.population)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "55")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE agent_id BETWEEN 901044 AND 901059 AND world_faction_id=2;"), "16")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE agent_id BETWEEN 901050 AND 901059 AND agent_type=5;"), "10")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agent_group_members WHERE member_agent_id BETWEEN 901044 AND 901059;"), "0")
        self.assertEqual(self.sql(all_identity), prepared)
        for query in queries:
            with self.subTest(query=query): self.assertEqual(self.sql(query), before[query])
        self.assertEqual(settlement.legacy_snapshot_digest(self.rows()), legacy)
        self.sql(defias.deactivation_sql(self.population))
        defias.validate_snapshot(self.rows(), self.population, control=0)

    def test_atomic_activation_refuses_new_or_old_actor_drift_without_disturbing_existing_forty(self):
        self.prepare()
        actor = self.population.recruits[0]
        mutations = (
            ("UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901044;", "UPDATE characters.ai_agents SET agent_type=1 WHERE agent_id=901044;"),
            ("UPDATE characters.ai_agents SET world_faction_id=1 WHERE agent_id=901050;", "UPDATE characters.ai_agents SET world_faction_id=2 WHERE agent_id=901050;"),
            ("UPDATE characters.ai_agents SET home_x=0 WHERE agent_id=901044;", f"UPDATE characters.ai_agents SET home_x={actor.x} WHERE agent_id=901044;"),
            ("UPDATE characters.ai_agents SET work_x=0 WHERE agent_id=901050;", "UPDATE characters.ai_agents SET work_x=NULL WHERE agent_id=901050;"),
            ("UPDATE world.ai_spawn_participation_defaults SET participation_mode=1 WHERE spawn_id=901059;", "UPDATE world.ai_spawn_participation_defaults SET participation_mode=0 WHERE spawn_id=901059;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,901050);", "DELETE FROM characters.ai_agent_group_members WHERE member_agent_id=901050;"),
            ("UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901016;", "UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901016;"),
            ("UPDATE characters.ai_agents SET agent_type=0 WHERE agent_id=901055;", "UPDATE characters.ai_agents SET agent_type=5 WHERE agent_id=901055;"))
        for mutate, restore in mutations:
            with self.subTest(mutation=mutate):
                self.sql(mutate)
                with self.assertRaises(defias.ProfileError): defias.validate_snapshot(self.rows(), self.population, control=None)
                self.sql(defias.activation_sql(self.population))
                self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id BETWEEN 901044 AND 901059 AND agent_id<>901055;"), "0")
                self.assertEqual(self.sql(f"SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND agent_id IN ({self.population.field_population.spawn_ids});"), "40")
                self.sql(restore)
        self.sql(defias.activation_sql(self.population))
        defias.validate_snapshot(self.rows(), self.population)


if __name__ == "__main__": unittest.main()
