"""Reviewed population boundaries, migration and real MySQL activation gates."""
from copy import deepcopy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import hunt_population as hunt, manage, single_return
from tools.realm_lab.tests.test_setup import environment
from tools.realm_lab.tests.test_single_return import fixture

ROOT = Path(__file__).resolve().parents[3]


def document():
    scope = single_return.Scope.load()
    source = single_return.load_json(single_return.METADATA / "test-points.json")["source_revision"]
    positions = ((266.667, 800), (326.667, 800), (291.667, 800),
                 (256.667, 826), (351.667, 800), (336.667, 774))
    return {
        "schema_version": 1, "profile": "hunt-cycle", "map_id": scope.map_id,
        "area_id": scope.area_id, "source_revision": source, "respawn_seconds": 120,
        "agents": [
            {"spawn_id": 900725 + index, "entry": 1186 if index < 2 else 883,
             "role": "predator" if index < 2 else "prey",
             "label": single_return.SPAWN_LABEL if index == 0 else f"aiworld_lab_hunt_{900725 + index}",
             "home": {"x": x, "y": y, "z": 0, "o": 0}}
            for index, (x, y) in enumerate(positions)
        ],
    }


def snapshot(population, control=0):
    scope = population.scope
    rows = []
    for entry, faction in ((1186, 44), (883, 31)):
        rows.append({"kind": "template", "entry": entry, "ai_name": "", "script_name": "",
                     "regenerate_health": 1, "vehicle": 0, "npcflag": 0, "unit_flags": 0, "faction": faction})
        rows.append({"kind": "type", "entry": entry, "type": 6 if entry == 1186 else 7})
    for actor in population.actors:
        rows.extend((
            {"kind": "spawn", "guid": actor.spawn_id, "entry": actor.entry, "map": scope.map_id,
             "zone": scope.area_id, "area": scope.area_id, "spawn_mask": 1, "phase_mask": 1,
             "x": actor.x, "y": actor.y, "z": actor.z, "orientation": actor.orientation,
             "movement": 0, "wander": 0, "label": actor.label, "script_name": "",
             "respawn": population.respawn_seconds, "npcflag": 0, "unit_flags": 0, "dynamicflags": 0},
            {"kind": "agent", "id": actor.spawn_id, "spawn": actor.spawn_id, "map": scope.map_id,
             "type": actor.agent_type, "control": control, "home_map": scope.map_id,
             "home_x": actor.x, "home_y": actor.y, "home_z": actor.z, "home_o": actor.orientation,
             "work_map": None, "faction": 0},
            {"kind": "participation", "spawn": actor.spawn_id, "mode": 0},
        ))
    return rows


class HuntPopulationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, self.data = fixture(Path(self.tmp.name))
        self.path = self.metadata / "hunt-population.json"
        self.write(document())
        self.population = hunt.Population.load(self.metadata)

    def write(self, value):
        self.path.write_text(json.dumps(value), encoding="utf-8")

    def test_verified_population_retains_map_proof_and_all_six_actor_whitelist(self):
        population = hunt.verify_data(self.data, self.metadata)
        self.assertEqual(population, self.population)
        self.assertEqual(population.config()["AIWorld.ScopeSpawnIds"], '"900725,900726,900727,900728,900729,900730"')
        self.assertEqual(population.observer_environment(), population.scope.observer_environment())
        self.assertEqual([actor.agent_type for actor in population.actors], [6, 6, 7, 7, 7, 7])
        self.assertEqual((population.actors[0].x, population.actors[0].y, population.actors[0].label),
                         (266.667, 800, single_return.SPAWN_LABEL))
        (self.data / "mmaps/7253130.mmtile").write_bytes(b"different")
        with self.assertRaisesRegex(hunt.ProfileError, "differs from reviewed bundle"):
            hunt.verify_data(self.data, self.metadata)

    def test_invalid_population_rejected_before_database_work(self):
        mutations = (
            lambda doc: doc.update(source_revision="unreviewed"),
            lambda doc: doc.update(map_id=0),
            lambda doc: doc.update(respawn_seconds=True),
            lambda doc: doc["agents"][0]["home"].update(x=267.667),
            lambda doc: doc["agents"][0].update(label="different"),
            lambda doc: doc["agents"][1].update(spawn_id=900725),
            lambda doc: doc["agents"][1]["home"].update(x=999),
            lambda doc: doc["agents"][1]["home"].update(z=1),
            lambda doc: doc["agents"][1]["home"].update(y=float("nan")),
            lambda doc: doc["agents"][2].update(role="predator"),
            lambda doc: doc["agents"].pop(),
            lambda doc: doc["agents"][2]["home"].update(x=268, y=800),
        )
        for index, mutate in enumerate(mutations):
            value = document()
            mutate(value)
            self.write(value)
            with self.subTest(case=index), self.assertRaises(hunt.ProfileError):
                hunt.Population.load(self.metadata)

    def test_migration_allows_existing_active_bear_and_demotes_all_six_to_observe(self):
        rows = snapshot(self.population)
        bear_only = deepcopy([row for row in rows if row["kind"] == "template" or
                              row["kind"] == "type" and row["entry"] == 1186 or
                              row.get("guid", row.get("spawn")) == 900725])
        next(row for row in bear_only if row["kind"] == "agent")["control"] = 1
        hunt.validate_snapshot(bear_only, self.population, control=None, bootstrap=True)
        with self.assertRaisesRegex(hunt.ProfileError, "incomplete"):
            hunt.validate_snapshot(bear_only, self.population, control=1)
        sql = hunt.bootstrap_sql(self.population)
        self.assertIn("SET control_mode=0", sql)
        self.assertIn("WHERE NOT EXISTS", sql)
        self.assertNotIn("UPDATE world.creature", sql)
        self.assertNotIn("home_x=", sql)
        hunt.validate_snapshot(rows, self.population, control=0)
        hunt.validate_snapshot(snapshot(self.population, 1), self.population, control=1)

    def test_readback_denies_home_role_group_external_actor_extra_spawn_or_respawn_drift(self):
        changes = (
            ("spawn", "x", 0), ("spawn", "respawn", 10), ("spawn", "unit_flags", 2),
            ("spawn", "map", 0), ("agent", "home_x", 0), ("agent", "type", 4),
            ("participation", "mode", 1), ("type", "type", 4), ("template", "ai_name", "SmartAI"),
            ("template", "unit_flags", 0x200),
        )
        for kind, field, value in changes:
            rows = snapshot(self.population)
            next(row for row in rows if row["kind"] == kind)[field] = value
            with self.subTest(kind=kind, field=field), self.assertRaises(hunt.ProfileError):
                hunt.validate_snapshot(rows, self.population, control=None, bootstrap=True)
        for extra in (
            {"kind": "membership", "group": 10, "member": 900725},
            dict(next(row for row in snapshot(self.population) if row["kind"] == "spawn"), guid=999),
            dict(next(row for row in snapshot(self.population) if row["kind"] == "agent"), id=999, spawn=999, map=0, control=1),
        ):
            with self.subTest(extra=extra["kind"]), self.assertRaises(hunt.ProfileError):
                hunt.validate_snapshot(snapshot(self.population) + [extra], self.population, control=None, bootstrap=True)

    def test_duplicate_or_truncated_sql_readback_and_partial_control_fail_closed(self):
        rows = snapshot(self.population)
        for bad in (rows + [deepcopy(rows[0])], [{"kind": "agent"}], rows[:-1]):
            with self.assertRaises(hunt.ProfileError):
                hunt.validate_snapshot(bad, self.population, control=0)
        next(row for row in rows if row["kind"] == "agent")["control"] = 1
        with self.assertRaisesRegex(hunt.ProfileError, "control mode"):
            hunt.validate_snapshot(rows, self.population, control=0)

    def test_generated_sql_never_mutates_auth_templates_homes_or_deletes_rows(self):
        for sql in (hunt.snapshot_sql(self.population), hunt.bootstrap_sql(self.population), hunt.activation_sql(self.population)):
            self.assertNotIn("auth.", sql)
            self.assertNotIn("DELETE", sql)
            self.assertNotIn("ALTER", sql)
            self.assertNotIn("UPDATE world.creature_template", sql)
        activation = hunt.activation_sql(self.population)
        self.assertIn("@lab_population_ready=1", activation)
        self.assertIn("COUNT(*)=6", activation)
        self.assertIn("control_mode=1)", activation)

    def test_hunt_profile_enables_natural_hunger_for_exact_population_with_other_systems_off(self):
        settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": "hunt-cycle"})
        config = manage.config_overrides(settings, self.population)
        self.assertEqual(config["AIWorld.ScopeSpawnIds"], '"900725,900726,900727,900728,900729,900730"')
        for key in ("Enable", "LivingRolesEnabled", "TelemetryEnabled", "LivingNeedEvolutionEnabled",
                    "ScopeRestrictAgents", "ScopeAlwaysActive"):
            self.assertEqual(config[f"AIWorld.{key}"], "1", key)
        self.assertEqual(config["AIWorld.NeedsHungerRatePerSecond"], "0.003")
        for key in ("NeedsFatigueRatePerSecond", "NeedsResourcePressureRatePerSecond", "RemoteInferenceEnabled",
                    "GroupCoarseSimulationEnabled", "RecoveryAdviceEnabled", "DynamicTaskEnable",
                    "CoalitionMaintenance", "GroupCoordination", "LivingRoleExtensionsEnabled", "TestSpawnId"):
            self.assertEqual(config[f"AIWorld.{key}"], "0", key)
        template = (ROOT / "deploy/worldserver.conf").read_text(encoding="utf-8")
        rendered = manage.render_config(template, settings, self.population)
        self.assertIn('AIWorld.ScopeSpawnIds = "900725,900726,900727,900728,900729,900730"\n', rendered)
        self.assertIn("AIWorld.LivingNeedEvolutionEnabled = 1\n", rendered)
        for profile in ("disabled", "single-return"):
            existing = manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile})
            defaults = manage.config_overrides(existing, self.population.scope)
            self.assertEqual(defaults["AIWorld.LivingNeedEvolutionEnabled"], "0")
            self.assertNotIn("AIWorld.NeedsHungerRatePerSecond", defaults)
            self.assertNotIn("AIWorld.NeedsFatigueRatePerSecond", defaults)
            self.assertNotIn("AIWorld.NeedsResourcePressureRatePerSecond", defaults)
        self.assertEqual((ROOT / "deploy/worldserver.conf").read_text(encoding="utf-8"), template)

    def test_failed_hunt_bundle_preflight_prevents_server_execution_and_all_sql(self):
        env = {**environment(), "LAB_AI_PROFILE": "hunt-cycle", "WORLD_VIEWER_TELEMETRY_TOKEN": "test-token"}
        with patch.dict(manage.os.environ, env), \
             patch.object(manage.sys, "argv", ["manage.py", "run-world", "unused.conf", "worldserver"]), \
             patch.object(hunt, "verify_data", side_effect=hunt.ProfileError("Hash mismatch")), \
             patch.object(manage, "lab_mysql") as private_sql, \
             patch.object(manage, "mysql") as auth_sql, \
             patch.object(manage.os, "execv") as execute:
            with self.assertRaises(SystemExit) as raised:
                manage.main()
            self.assertEqual(raised.exception.code, 2)
            private_sql.assert_not_called()
            auth_sql.assert_not_called()
            execute.assert_not_called()

    def test_bootstrap_and_activation_use_only_private_lab_sql_with_validated_readback(self):
        settings = manage.Settings.load(environment())
        initial = [row for row in snapshot(self.population, 1) if row["kind"] == "template" or
                   row["kind"] == "type" and row["entry"] == 1186 or
                   row.get("guid", row.get("spawn")) == 900725]
        for function, before, after in ((manage.bootstrap_hunt, initial, snapshot(self.population)),
                                        (manage.activate_hunt, snapshot(self.population), snapshot(self.population, 1))):
            outputs = ["\n".join(json.dumps(row) for row in before), "",
                       "\n".join(json.dumps(row) for row in after)]
            results = [type("Result", (), {"returncode": 0, "stdout": output})() for output in outputs]
            with self.subTest(function=function.__name__), \
                 patch.object(manage.subprocess, "run", side_effect=results) as run, \
                 patch.object(manage, "mysql") as auth_sql:
                function(self.data, settings, self.metadata)
                self.assertEqual(run.call_count, 3)
                auth_sql.assert_not_called()
                for call in run.call_args_list:
                    args, kwargs = call
                    self.assertIn("--host=lab-mysql", args[0])
                    self.assertEqual(args[0][-1], "world")
                    self.assertEqual(kwargs["env"]["MYSQL_PWD"], settings.db_password)
                    self.assertNotIn("auth.", kwargs["input"])
                    self.assertNotIn(settings.auth_password, kwargs["input"])
                mutation = run.call_args_list[1].kwargs["input"]
                self.assertIn("START TRANSACTION", mutation)
                self.assertIn("control_mode=0" if function is manage.bootstrap_hunt else "control_mode=1", mutation)

    def test_enabled_profiles_reject_population_mutation_before_preflight_or_sql(self):
        for profile in ("single-return", "hunt-cycle"):
            settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": profile})
            for function in (manage.bootstrap_hunt, manage.activate_hunt):
                with self.subTest(profile=profile, function=function.__name__), \
                     patch.object(manage, "population_preflight") as preflight, \
                     patch.object(manage, "lab_mysql") as private_sql, \
                     patch.object(manage, "mysql") as auth_sql:
                    with self.assertRaisesRegex(manage.SetupError, "LAB_AI_PROFILE=disabled"):
                        function(self.data, settings, self.metadata)
                    preflight.assert_not_called()
                    private_sql.assert_not_called()
                    auth_sql.assert_not_called()

    def test_collision_readback_prevents_bootstrap_mutation(self):
        rows = snapshot(self.population)
        next(row for row in rows if row["kind"] == "spawn")["label"] = "foreign_spawn"
        result = type("Result", (), {"returncode": 0, "stdout": "\n".join(json.dumps(row) for row in rows)})()
        with patch.object(manage.subprocess, "run", return_value=result) as run:
            with self.assertRaisesRegex(hunt.ProfileError, "collision"):
                manage.bootstrap_hunt(self.data, manage.Settings.load(environment()), self.metadata)
            self.assertEqual(run.call_count, 1)
            self.assertNotIn("START TRANSACTION", run.call_args.kwargs["input"])


@unittest.skipUnless(os.getenv("LAB_TEST_MYSQL") == "1", "Disposable MySQL integration is a CI gate")
class HuntPopulationMySQLTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.metadata, _ = fixture(Path(self.tmp.name))
        (self.metadata / "hunt-population.json").write_text(json.dumps(document()), encoding="utf-8")
        self.population = hunt.Population.load(self.metadata)
        self.admin = manage.Settings.load({**environment(), "TC_AUTH_DB_HOST": "127.0.0.1",
                                          "TC_AUTH_DB_USER": "root",
                                          "TC_AUTH_DB_PASSWORD": os.environ["LAB_TEST_MYSQL_ROOT_PASSWORD"]})
        self.sql("""CREATE DATABASE IF NOT EXISTS world;
CREATE DATABASE IF NOT EXISTS characters;
DROP TABLE IF EXISTS world.creature, world.creature_template,
 world.ai_spawn_participation_defaults, world.ai_agent_type_entry_defaults,
 characters.ai_agents, characters.ai_agent_group_members;
CREATE TABLE world.creature_template (
 entry INT PRIMARY KEY, AIName VARCHAR(64), ScriptName VARCHAR(64), RegenHealth INT, VehicleId INT,
 npcflag INT, unit_flags INT UNSIGNED, faction INT);
CREATE TABLE world.creature (
 guid BIGINT PRIMARY KEY, id INT, map INT, zoneId INT, areaId INT, spawnMask INT, phaseMask INT,
 position_x FLOAT, position_y FLOAT, position_z FLOAT, orientation FLOAT, spawntimesecs INT DEFAULT 120,
 wander_distance FLOAT, MovementType INT, curhealth INT DEFAULT 1, curmana INT DEFAULT 0,
 StringId VARCHAR(64), VerifiedBuild INT, ScriptName VARCHAR(64) NOT NULL DEFAULT '',
 npcflag INT DEFAULT 0, unit_flags INT UNSIGNED DEFAULT 0, dynamicflags INT DEFAULT 0);
CREATE TABLE world.ai_spawn_participation_defaults (spawn_id BIGINT PRIMARY KEY, participation_mode INT);
CREATE TABLE world.ai_agent_type_entry_defaults (creature_entry INT PRIMARY KEY, agent_type INT);
CREATE TABLE characters.ai_agents (
 agent_id BIGINT PRIMARY KEY, agent_type INT, map_id INT, spawn_id BIGINT, control_mode INT,
 home_map_id INT, home_x FLOAT, home_y FLOAT, home_z FLOAT, home_o FLOAT,
 work_map_id INT NULL, world_faction_id INT, UNIQUE(map_id,spawn_id));
CREATE TABLE characters.ai_agent_group_members (group_id BIGINT, member_agent_id BIGINT);
INSERT INTO world.creature_template VALUES (1186,'','',1,0,0,0,44),(883,'','',1,0,0,0,31);
""")
        self.auth_tables = self.sql("SHOW TABLES FROM auth;")

    def sql(self, sql):
        return manage.mysql(self.admin, sql)

    def rows(self):
        return hunt.parse_snapshot(self.sql(hunt.snapshot_sql(self.population)))

    def prepare(self):
        hunt.validate_snapshot(self.rows(), self.population, control=None, bootstrap=True)
        self.sql(hunt.bootstrap_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=0)

    def assert_inactive(self):
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "0")

    def test_migrates_active_single_bear_idempotently_and_controls_only_reviewed_six(self):
        scope = self.population.scope
        self.sql(single_return.bootstrap_sql(scope))
        self.sql(single_return.activation_sql(scope))
        original = self.sql("SELECT guid,id,map,position_x,position_y,position_z,StringId,curhealth FROM world.creature WHERE guid=900725;")
        self.assertEqual(self.sql("SELECT control_mode FROM characters.ai_agents WHERE agent_id=900725;"), "1")
        self.prepare()
        self.sql(hunt.bootstrap_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=0)
        self.assertEqual(self.sql("SELECT guid,id,map,position_x,position_y,position_z,StringId,curhealth FROM world.creature WHERE guid=900725;"), original)
        self.sql(hunt.activation_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=1)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM characters.ai_agents WHERE control_mode=1;"), "6")
        self.assertEqual(self.sql("SHOW TABLES FROM auth;"), self.auth_tables)

    def test_sql_activation_is_all_or_none_when_one_home_group_or_template_is_invalid(self):
        self.prepare()
        mutations = (
            ("UPDATE characters.ai_agents SET home_x=100 WHERE agent_id=900730;",
             "UPDATE characters.ai_agents SET home_x=336.667 WHERE agent_id=900730;"),
            ("INSERT INTO characters.ai_agent_group_members VALUES (1,900730);",
             "TRUNCATE TABLE characters.ai_agent_group_members;"),
            ("UPDATE world.creature_template SET AIName='SmartAI' WHERE entry=883;",
             "UPDATE world.creature_template SET AIName='' WHERE entry=883;"),
            ("UPDATE world.creature SET spawntimesecs=10 WHERE guid=900727;",
             "UPDATE world.creature SET spawntimesecs=120 WHERE guid=900727;"),
        )
        for bad, restore in mutations:
            with self.subTest(mutation=bad):
                self.sql(bad)
                self.sql(hunt.activation_sql(self.population))
                self.assert_inactive()
                self.sql(restore)
        self.sql(hunt.activation_sql(self.population))
        hunt.validate_snapshot(self.rows(), self.population, control=1)

    def test_outside_controlled_agent_or_unexpected_map_spawn_blocks_all_six(self):
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
