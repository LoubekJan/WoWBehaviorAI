import copy
from dataclasses import replace
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.realm_lab import manage, settlement_population as settlement, world_setup
from tools.realm_lab.tests.test_setup import environment


ROOT = Path(__file__).resolve().parents[3]


def before_rows():
    return [{"race": 1, "class": 1, "map": 0, "zone": 12, "x": -8949.95, "y": -132.493,
             "z": 83.5312, "orientation": 0.0},
            {"race": 2, "class": 1, "map": 1, "zone": 14, "x": -618.518, "y": -4251.67,
             "z": 38.718, "orientation": 0.0}]


def after_rows(start):
    return [{**row, "map": start.map_id, "zone": start.area_id,
             **{field: getattr(start, field) for field in ("x", "y", "z", "orientation")}} for row in before_rows()]


class CanonicalWorldSetupTests(unittest.TestCase):
    def setUp(self):
        self.population = settlement.Population.load()
        self.start = world_setup.WorldStart.load(self.population)
        self.settings = manage.Settings.load({**environment(), "LAB_AI_PROFILE": settlement.PROFILE})
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def test_registration_removes_only_an_unused_schema_default_then_registers_realm_two(self):
        candidate = json.dumps({"name": "Trinity", "address": "127.0.0.1", "local_address": "127.0.0.1",
                                "subnet": "255.255.255.0", "port": 8085, "build": 12340})
        expected = "2\tAI World Lab\t192.168.0.248\t192.168.0.248\t255.255.255.0\t9086"
        with patch.object(manage, "mysql", side_effect=[candidate, "0", "", "0", expected]) as sql:
            world_setup.register_world(self.settings)
        removal = sql.call_args_list[2].args[1]
        self.assertIn("r.id=1 AND r.name='Trinity'", removal)
        self.assertIn("empty_counts.numchars=0", removal)
        self.assertIn("occupied.numchars<>0", removal)
        self.assertNotIn("DELETE FROM account", removal)
        self.assertIn("WHERE id=2 AND name='AI World Lab'", sql.call_args_list[-1].args[1])

    def test_registration_preserves_existing_realm_and_rejects_used_placeholder(self):
        default = {"name": "Trinity", "address": "127.0.0.1", "local_address": "127.0.0.1",
                   "subnet": "255.255.255.0", "port": 8085, "build": 12340}
        for candidate, count, calls in (({**default, "name": "Elwynn"}, "0", 1), (default, "1", 2)):
            with patch.object(manage, "mysql", side_effect=[json.dumps(candidate), count]) as sql:
                with self.subTest(), self.assertRaises(manage.SetupError):
                    world_setup.register_world(self.settings)
            self.assertEqual(sql.call_count, calls)
            self.assertTrue(all("DELETE" not in call.args[1] for call in sql.call_args_list))

    def test_registration_is_repeatable_without_realm_one(self):
        expected = "2\tAI World Lab\t192.168.0.248\t192.168.0.248\t255.255.255.0\t9086"
        with patch.object(manage, "mysql", side_effect=["", expected]) as sql:
            world_setup.register_world(self.settings)
        self.assertEqual(sql.call_count, 2)

    def test_versioned_entry_matches_original_verified_v8_position(self):
        self.assertEqual((self.start.map_id, self.start.area_id, self.start.x, self.start.y), (725, 4988, 266.667, 800))
        self.assertAlmostEqual(self.start.z, 2.015391923194993)
        self.assertEqual(world_setup.geometry_digest(self.population.geometry_files),
                         json.loads(world_setup.START_METADATA.read_text())["geometry_sha256"])

    def test_entry_rejects_changed_geometry_and_missing_ground_proof(self):
        original = json.loads(world_setup.START_METADATA.read_text())
        for field, value in (("geometry_sha256", "0" * 64), ("map_id", 0), ("z", float("nan")),
                             ("orientation", -1)):
            document = {**original, field: value}
            path = self.directory / "entry.json"
            path.write_text(json.dumps(document))
            with self.subTest(field=field), self.assertRaises(manage.SetupError):
                world_setup.WorldStart.load(self.population, path)
        for key in ("dry", "outside_collision_bounds", "outside_terrain_holes"):
            document = copy.deepcopy(original)
            document["evidence"][key] = False
            path.write_text(json.dumps(document))
            with self.subTest(key=key), self.assertRaises(manage.SetupError):
                world_setup.WorldStart.load(self.population, path)

    def test_entry_rejects_spawn_within_native_aggro_radius(self):
        nearby = replace(self.population.actors[0], x=self.start.x, y=self.start.y)
        with self.assertRaisesRegex(manage.SetupError, "too close"):
            world_setup.WorldStart.load(replace(self.population, actors=(nearby, *self.population.actors[1:])))

    def test_schema_update_uses_update_only_and_disables_only_reviewed_retired_pilots(self):
        target = self.directory / "schema.conf"
        original_path = Path
        with patch("tools.realm_lab.world_setup.Path", side_effect=lambda path: target if str(path) == "/tmp/world-schema-setup.conf" else original_path(path)), \
             patch.object(settlement, "ensure_world_offline") as offline, \
             patch.object(settlement, "verify_data", return_value=self.population), \
             patch.object(manage, "preflight"), \
             patch.object(manage, "lab_mysql", side_effect=["", "0"]) as sql, \
             patch("tools.realm_lab.world_setup.subprocess.run") as process:
            process.return_value.returncode = 0
            world_setup.prepare(self.directory, ROOT / "deploy/worldserver.conf", Path("/build/bin/worldserver"), self.settings)
        process.assert_called_once_with([str(Path("/build/bin/worldserver")), "--update-databases-only", "-c", str(target)],
                                        capture_output=True, text=True, check=False)
        self.assertGreaterEqual(offline.call_count, 4)
        demotion = sql.call_args_list[0].args[1]
        self.assertIn("map_id=0 AND agent_id=spawn_id", demotion)
        self.assertIn("80335,80683,214021,214023", demotion)
        self.assertNotIn("DELETE", demotion)
        self.assertNotIn("900725", demotion)
        self.assertIn("Updates.EnableDatabases = 6\n", target.read_text())

    def test_failed_schema_process_never_runs_pilot_demotion(self):
        with patch.object(world_setup, "validate_profile"), \
             patch.object(settlement, "ensure_world_offline"), \
             patch.object(settlement, "verify_data", return_value=self.population), \
             patch.object(manage, "preflight"), \
             patch("tools.realm_lab.world_setup.os.open", side_effect=OSError("unwritable target")), \
             patch.object(manage, "lab_mysql") as sql:
            with self.assertRaises(OSError):
                world_setup.prepare(self.directory, ROOT / "deploy/worldserver.conf", Path("server"), self.settings)
        sql.assert_not_called()

    def test_unexpected_managed_agent_fails_after_narrow_pilot_demotion(self):
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(manage, "lab_mysql", side_effect=["", "1"]):
            with self.assertRaisesRegex(manage.SetupError, "Unexpected managed"):
                world_setup.demote_retired_pilots(self.settings)

    def test_apply_start_saves_before_image_and_only_changes_world_playercreateinfo(self):
        receipt = self.directory / "before.json"

        def execute(settings, sql):
            self.assertEqual(json.loads(receipt.read_text())["before"], before_rows())
            self.assertIn("UPDATE world.playercreateinfo", sql)
            self.assertIn("WHERE (race,class) IN ((1,1),(2,1))", sql)
            self.assertNotIn("characters.", sql)
            self.assertNotIn("auth.", sql)
            self.assertNotIn("DELETE", sql)
            return ""

        with patch.object(settlement, "ensure_world_offline") as offline, \
             patch.object(settlement, "verify_data", return_value=self.population), \
             patch.object(world_setup, "starts_snapshot", side_effect=[before_rows(), after_rows(self.start)]), \
             patch.object(manage, "lab_mysql", side_effect=execute) as sql:
            world_setup.apply_start(self.directory, receipt, self.settings)
        sql.assert_called_once()
        self.assertEqual(offline.call_count, 2)
        record = json.loads(receipt.read_text())
        self.assertEqual(record["status"], "verified")
        self.assertFalse(record["existing_characters_modified"])

    def test_existing_receipt_blocks_mutation_and_is_not_overwritten(self):
        receipt = self.directory / "before.json"
        receipt.write_text("original")
        with patch.object(settlement, "ensure_world_offline"), \
             patch.object(settlement, "verify_data", return_value=self.population), \
             patch.object(world_setup, "starts_snapshot", return_value=before_rows()), \
             patch.object(manage, "lab_mysql") as sql:
            with self.assertRaises(FileExistsError):
                world_setup.apply_start(self.directory, receipt, self.settings)
        sql.assert_not_called()
        self.assertEqual(receipt.read_text(), "original")

    def test_start_readback_and_profile_guards_reject_drift(self):
        for rows in (after_rows(self.start)[:1], before_rows()):
            with self.subTest(), self.assertRaises(manage.SetupError):
                world_setup.verify_starts(before_rows(), rows, self.start)
        for settings in (replace(self.settings, realm_id=3), replace(self.settings, ai_profile="disabled")):
            with self.subTest(), self.assertRaises(manage.SetupError):
                world_setup.validate_profile(settings)

    def test_empty_or_invalid_tdb_starts_fail_before_sql_generation(self):
        for rows in ([], [{**before_rows()[0], "race": "1); DROP TABLE characters"}],
                     [before_rows()[0], before_rows()[0]]):
            raw = "\n".join(json.dumps(row) for row in rows)
            with patch.object(manage, "lab_mysql", return_value=raw), self.subTest(), self.assertRaises(manage.SetupError):
                world_setup.starts_snapshot(self.settings)

    def test_valid_tdb_snapshot_preserves_exact_race_class_identities(self):
        raw = "\n".join(json.dumps(row) for row in before_rows())
        with patch.object(manage, "lab_mysql", return_value=raw):
            self.assertEqual(world_setup.starts_snapshot(self.settings), before_rows())


if __name__ == "__main__":
    unittest.main()
