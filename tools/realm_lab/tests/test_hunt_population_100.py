"""Reviewed 100-actor scope and an offline six-to-100 MySQL migration."""
import json
import math
import os
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import hunt_population as hunt, single_return
from tools.realm_lab.tests import test_hunt_population as small_tests
from tools.realm_lab.tests.test_single_return import fixture

ROOT = Path(__file__).resolve().parents[3]
small_document = small_tests.document
snapshot = small_tests.snapshot


def document():
    return single_return.load_json(single_return.METADATA / "hunt-population-100.json")


class HuntPopulation100Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data = fixture(Path(self.tmp.name))
        (self.metadata / "hunt-population.json").write_text(json.dumps(small_document()), encoding="utf-8")
        self.path = self.metadata / "hunt-population-100.json"
        self.write(document())
        self.population = hunt.Population.load(self.metadata, profile="hunt-100")

    def write(self, value):
        self.path.write_text(json.dumps(value), encoding="utf-8")

    def test_explicit_large_profile_keeps_default_six_profile_and_same_verified_geometry(self):
        small = hunt.Population.load(self.metadata)
        large = hunt.verify_data(self.data, self.metadata, profile="hunt-100")
        self.assertEqual(large, self.population)
        self.assertEqual((small.profile, len(small.actors)), ("hunt-cycle", 6))
        self.assertEqual((large.profile, len(large.actors)), ("hunt-100", 100))
        self.assertEqual(large.scope, small.scope)
        self.assertEqual(large.observer_environment(), small.observer_environment())
        self.assertEqual(large.actors[:6], small.actors)
        self.assertEqual({actor.spawn_id for actor in large.actors}, set(range(900725, 900825)))
        self.assertEqual([actor.role for actor in large.actors].count("predator"), 20)
        self.assertEqual([actor.role for actor in large.actors].count("prey"), 80)
        expected = '"' + ",".join(str(spawn) for spawn in range(900725, 900825)) + '"'
        self.assertEqual(large.config()["AIWorld.ScopeSpawnIds"], expected)
        self.assertEqual(small.config()["AIWorld.ScopeSpawnIds"], '"900725,900726,900727,900728,900729,900730"')

    def test_large_population_still_requires_live_map_hashes_before_database_work(self):
        (self.data / "mmaps/7253130.mmtile").write_bytes(b"unreviewed navigation")
        with self.assertRaisesRegex(hunt.ProfileError, "differs from reviewed bundle"):
            hunt.verify_data(self.data, self.metadata, profile="hunt-100")

    def test_every_predator_has_two_prey_homes_within_native_scan_distance(self):
        predators = [actor for actor in self.population.actors if actor.role == "predator"]
        prey = [actor for actor in self.population.actors if actor.role == "prey"]
        for predator in predators:
            distances = [math.hypot(predator.x - actor.x, predator.y - actor.y) for actor in prey]
            with self.subTest(predator=predator.spawn_id):
                self.assertGreaterEqual(sum(distance <= 25 for distance in distances), 2)
        for index, actor in enumerate(self.population.actors):
            for other in self.population.actors[index + 1:]:
                self.assertGreaterEqual(math.hypot(actor.x - other.x, actor.y - other.y), 5)

    def test_original_six_identity_is_not_redefined_by_another_metadata_file(self):
        value = document()
        value["agents"][1]["home"]["x"] -= 1
        small = small_document()
        small["agents"][1]["home"]["x"] -= 1
        self.write(value)
        (self.metadata / "hunt-population.json").write_text(json.dumps(small), encoding="utf-8")
        with self.assertRaisesRegex(hunt.ProfileError, "all six original"):
            hunt.Population.load(self.metadata, profile="hunt-100")
        value = document()
        # Retain valid totals and template/role pairs while moving an original role.
        for row in (value["agents"][1], value["agents"][-1]):
            row["role"] = "prey" if row["role"] == "predator" else "predator"
            row["entry"] = hunt.ROLE_ENTRIES[row["role"]]
        self.write(value)
        with self.assertRaisesRegex(hunt.ProfileError, "all six original"):
            hunt.Population.load(self.metadata, profile="hunt-100")

    def test_count_roles_identity_bounds_and_overlap_fail_closed(self):
        mutations = (
            lambda doc: doc.update(profile="hunt-cycle"),
            lambda doc: doc.update(schema_version=2),
            lambda doc: doc.update(source_revision="v3"),
            lambda doc: doc["agents"].pop(),
            lambda doc: doc["agents"][-1].update(spawn_id=900825),
            lambda doc: doc["agents"][-1].update(spawn_id=900823),
            lambda doc: doc["agents"][-1].update(entry=1186, role="predator"),
            lambda doc: doc["agents"][-1].update(label="foreign_npc"),
            lambda doc: doc["agents"][-1]["home"].update(x=366.668),
            lambda doc: doc["agents"][-1]["home"].update(y=900.001),
            lambda doc: doc["agents"][-1]["home"].update(z=1),
            lambda doc: doc["agents"][-1]["home"].update(x=float("inf")),
            lambda doc: doc["agents"][-1]["home"].update(x=266.667, y=800),
            lambda doc: doc["agents"][10]["home"].update(x=366.667, y=900),
        )
        for index, mutate in enumerate(mutations):
            value = document()
            mutate(value)
            self.write(value)
            with self.subTest(case=index), self.assertRaises(hunt.ProfileError):
                hunt.Population.load(self.metadata, profile="hunt-100")
        for unknown in ("hunt-1000", "", None, []):
            with self.subTest(profile=unknown), self.assertRaises(hunt.ProfileError):
                hunt.Population.load(self.metadata, profile=unknown)

    def test_offline_migration_accepts_active_six_then_requires_every_new_actor(self):
        small = hunt.Population.load(self.metadata)
        hunt.validate_snapshot(snapshot(small, 1), self.population, control=None, bootstrap=True)
        with self.assertRaisesRegex(hunt.ProfileError, "incomplete"):
            hunt.validate_snapshot(snapshot(small, 1), self.population, control=1)
        hunt.validate_snapshot(snapshot(self.population), self.population, control=0)
        hunt.validate_snapshot(snapshot(self.population, 1), self.population, control=1)
        for invalid in (
            dict(next(row for row in snapshot(small) if row["kind"] == "agent"), id=999, spawn=999, map=0, control=1),
            dict(next(row for row in snapshot(small) if row["kind"] == "spawn"), guid=999),
        ):
            with self.subTest(kind=invalid["kind"]), self.assertRaises(hunt.ProfileError):
                hunt.validate_snapshot(snapshot(small, 1) + [invalid], self.population, control=None, bootstrap=True)

    def test_last_actor_drift_or_partial_activation_is_rejected_in_readback(self):
        for field, value in (("home_x", 0), ("type", 7), ("control", 1)):
            rows = snapshot(self.population)
            row = next(row for row in rows if row["kind"] == "agent" and row["id"] == 900824)
            # The last actor is already prey; use a predator type to test drift.
            row[field] = 6 if field == "type" else value
            with self.subTest(field=field), self.assertRaises(hunt.ProfileError):
                hunt.validate_snapshot(rows, self.population, control=0)

    def test_large_sql_uses_one_activation_gate_and_preserves_existing_rows(self):
        bootstrap = hunt.bootstrap_sql(self.population)
        activation = hunt.activation_sql(self.population)
        self.assertEqual(bootstrap.count("INSERT INTO world.creature ("), 100)
        self.assertEqual(bootstrap.count("INSERT INTO characters.ai_agents ("), 100)
        self.assertEqual(activation.count("UPDATE characters.ai_agents"), 1)
        self.assertIn("COUNT(*)=100", activation)
        self.assertIn("@lab_population_ready=1", activation)
        self.assertIn("a.agent_id=900824 AND a.spawn_id=900824", activation)
        for sql in (bootstrap, activation):
            self.assertNotIn("auth.", sql)
            self.assertNotIn("DELETE", sql)
            self.assertNotIn("UPDATE world.creature", sql)
            self.assertNotIn("SET home_", sql)


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class HuntPopulation100MySQLTests(unittest.TestCase):
    # The original fixture creates disposable world/characters tables. These
    # tests are deliberately gated exactly like the six-actor integration tests.
    sql = small_tests.HuntPopulationMySQLTests.sql
    rows = small_tests.HuntPopulationMySQLTests.rows
    prepare = small_tests.HuntPopulationMySQLTests.prepare
    assert_inactive = small_tests.HuntPopulationMySQLTests.assert_inactive

    def setUp(self):
        small_tests.HuntPopulationMySQLTests.setUp(self)
        self.small = self.population
        (self.metadata / "hunt-population-100.json").write_text(json.dumps(document()), encoding="utf-8")
        self.population = hunt.Population.load(self.metadata, profile="hunt-100")

    def seed_active_six(self):
        self.sql(hunt.bootstrap_sql(self.small))
        self.sql(hunt.activation_sql(self.small))
        hunt.validate_snapshot(hunt.parse_snapshot(self.sql(hunt.snapshot_sql(self.small))), self.small, control=1)

    def test_active_six_migrate_to_100_without_replacing_homes_identity_or_health(self):
        self.seed_active_six()
        self.sql("UPDATE world.creature SET curhealth=guid-900700,curmana=5 WHERE guid BETWEEN 900725 AND 900730;")
        spawn_query = """SELECT guid,id,map,position_x,position_y,position_z,orientation,StringId,curhealth,curmana
 FROM world.creature WHERE guid BETWEEN 900725 AND 900730 ORDER BY guid;"""
        agent_query = """SELECT agent_id,agent_type,map_id,spawn_id,home_map_id,home_x,home_y,home_z,home_o,work_map_id,world_faction_id
 FROM characters.ai_agents WHERE agent_id BETWEEN 900725 AND 900730 ORDER BY agent_id;"""
        original_spawns, original_agents = self.sql(spawn_query), self.sql(agent_query)
        self.prepare()
        self.sql(hunt.bootstrap_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=0)
        self.assertEqual(self.sql(spawn_query), original_spawns)
        self.assertEqual(self.sql(agent_query), original_agents)
        self.sql(hunt.activation_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=1)
        self.assertEqual(self.sql("SELECT agent_type,COUNT(*) FROM characters.ai_agents WHERE control_mode=1 GROUP BY agent_type ORDER BY agent_type;"),
                         "6\t20\n7\t80")
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.auth_tables)

    def test_sql_gate_denies_every_actor_when_last_actor_is_invalid(self):
        self.prepare()
        actor = self.population.actors[-1]
        mutations = (
            ("UPDATE characters.ai_agents SET home_x=100 WHERE agent_id=900824;",
             f"UPDATE characters.ai_agents SET home_x={actor.x} WHERE agent_id=900824;"),
            ("UPDATE characters.ai_agents SET agent_type=6 WHERE agent_id=900824;",
             "UPDATE characters.ai_agents SET agent_type=7 WHERE agent_id=900824;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,900824);",
             "TRUNCATE TABLE characters.ai_agent_group_members;"),
            ("UPDATE world.creature SET unit_flags=2 WHERE guid=900824;",
             "UPDATE world.creature SET unit_flags=0 WHERE guid=900824;"),
        )
        for bad, restore in mutations:
            with self.subTest(mutation=bad):
                self.sql(bad)
                self.sql(hunt.activation_sql(self.population))
                self.assert_inactive()
                self.sql(restore)
        self.sql(hunt.activation_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=1)

    def test_foreign_controlled_agent_or_extra_map_spawn_prevents_100_activation(self):
        self.prepare()
        self.sql("INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode) VALUES (999,6,0,999,1);")
        self.sql(hunt.activation_sql(self.population))
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1 AND map_id=725;"), "0")
        self.sql("UPDATE characters.ai_agents SET control_mode=0 WHERE agent_id=999;")
        self.sql("INSERT INTO world.creature (guid,id,map) VALUES (999,1186,725);")
        self.sql(hunt.activation_sql(self.population))
        self.assert_inactive()
        with self.assertRaises(hunt.ProfileError):
            hunt.validate_snapshot(self.rows(), self.population, control=0)


if __name__ == "__main__":
    unittest.main()
