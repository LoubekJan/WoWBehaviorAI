from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from app.targets import TargetRegistry


ROOT = Path(__file__).resolve().parents[3]


class ReviewedTargetRegistryTests(unittest.TestCase):
    def test_actual_fields_population_accepts_only_the_existing_controlled_cohort(self):
        registry = TargetRegistry.load(ROOT, "fields-v8")
        self.assertTrue(registry.available, registry.reason)
        self.assertEqual(len(registry.targets), 100)
        self.assertEqual(sum(row.eligible for row in registry.targets), 40)
        for spawn in (901016, 901035, 901036, 901039, 901055):
            self.assertTrue(registry.validate(str(spawn), str(spawn), 725)["eligible"])
        denied = registry.validate("901014", "901014", 725)
        self.assertFalse(denied["eligible"])
        self.assertIn("běžné herní AI", denied["reason"])
        self.assertEqual(denied["target"]["control_mode"], "OBSERVE_ONLY")

    def test_scope_changes_with_reviewed_profile_without_broadening_native_ownership(self):
        for profile, expected in (("consumers-v8", 26), ("raids-v8", 28), ("fields-v8", 40), ("defias-v8", 55)):
            with self.subTest(profile=profile):
                registry = TargetRegistry.load(ROOT, profile)
                self.assertTrue(registry.available, registry.reason)
                self.assertEqual(sum(row.eligible for row in registry.targets), expected)
                self.assertFalse(registry.validate("901014", "901014", 725)["eligible"])
        self.assertFalse(TargetRegistry.load(ROOT, "consumers-v8").validate("901016", "901016", 725)["eligible"])

    def test_defias_overlay_admits_only_its_reviewed_extra_actors(self):
        previous = TargetRegistry.load(ROOT, "fields-v8")
        current = TargetRegistry.load(ROOT, "defias-v8")
        self.assertTrue(current.available, current.reason)
        self.assertEqual(len(current.targets), 100)
        original_ids = {row.spawn_id for row in previous.targets if row.eligible}
        current_ids = {row.spawn_id for row in current.targets if row.eligible}
        self.assertEqual(current_ids - original_ids, set(range(901044, 901060)) - {901055})
        for spawn in (901044, 901048, 901050, 901059):
            self.assertTrue(current.validate(str(spawn), str(spawn), 725)["eligible"])
            self.assertFalse(previous.validate(str(spawn), str(spawn), 725)["eligible"])
        self.assertFalse(current.validate("901014", "901014", 725)["eligible"])
        self.assertEqual(current.validate("901048", "901048", 725)["target"]["name"], "Zásobovač · Defias")

    def test_full_population_admits_every_reviewed_actor_without_inventing_new_targets(self):
        previous = TargetRegistry.load(ROOT, "defias-v8")
        current = TargetRegistry.load(ROOT, "full-v8")
        self.assertTrue(current.available, current.reason)
        self.assertEqual({row.spawn_id for row in current.targets}, set(range(901000, 901100)))
        self.assertTrue(all(row.eligible and row.control_mode == "AI_WORLD_CONTROLLED" for row in current.targets))
        self.assertEqual(sum(row.eligible for row in current.targets), 100)
        self.assertEqual(sum(row.eligible for row in previous.targets), 55)
        for spawn in range(901000, 901100):
            with self.subTest(spawn=spawn):
                verified = current.validate(str(spawn), str(spawn), 725)
                self.assertTrue(verified["eligible"])
                self.assertNotEqual(verified["target"]["living_role"], "NONE")
        self.assertFalse(previous.validate("901014", "901014", 725)["eligible"])
        self.assertEqual(current.validate("901014", "901014", 725)["target"]["entry"],
                         previous.validate("901014", "901014", 725)["target"]["entry"])
        for identity in ("900999", "901100", "18446744073709551615"):
            self.assertFalse(current.validate(identity, identity, 725)["eligible"])
        self.assertFalse(current.validate("901014", "901014", 0)["eligible"])
        self.assertFalse(current.validate("901014", "901015", 725)["eligible"])

    def test_unknown_identity_and_wrong_map_are_not_verified_by_numeric_syntax(self):
        registry = TargetRegistry.load(ROOT, "fields-v8")
        self.assertFalse(registry.validate("18446744073709551615", "18446744073709551615", 725)["eligible"])
        mismatch = registry.validate("901035", "901035", 0)
        self.assertFalse(mismatch["eligible"])
        self.assertEqual(mismatch["target"]["map_id"], 725)
        self.assertFalse(registry.validate("901035", "901036", 725)["eligible"])

    def test_missing_or_unsupported_metadata_fails_closed_with_no_raw_path(self):
        with tempfile.TemporaryDirectory() as folder:
            for profile in ("fields-v8", "settlement-v8", "untrusted-profile"):
                registry = TargetRegistry.load(Path(folder), profile)
                self.assertFalse(registry.available)
                self.assertFalse(registry.validate("901035", "901035", 725)["eligible"])
                self.assertNotIn(folder, registry.reason)

    def test_a_rejected_population_cannot_supply_partial_or_stale_targets(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "tools/realm_lab/field_population.py"
            source.parent.mkdir(parents=True)
            source.write_text("", encoding="utf-8")
            (root / "data/realm_lab/aiworldlab/preview-v8").mkdir(parents=True)

            class InvalidPopulation:
                @staticmethod
                def load(_metadata):
                    raise ValueError("Private failing path must not be exposed")

            module = SimpleNamespace(Population=InvalidPopulation, __file__=str(source))
            with patch("app.targets.importlib.import_module", return_value=module):
                registry = TargetRegistry.load(root, "fields-v8")
            self.assertFalse(registry.available)
            self.assertFalse(registry.targets)
            self.assertNotIn("Private", registry.reason)


if __name__ == "__main__":
    unittest.main()
