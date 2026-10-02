"""Recovery protocol regressions requiring only the production Pydantic model."""
import json
import unittest

from pydantic import ValidationError

from app.telemetry import ReturnRecovery


def legacy_recovery():
    return dict(failures=0, trail_points=0, retry_ms=0, stalled_ms=0, strategy="HOME_PATH",
                failure="NONE", candidates=0, path_type=0, requested_z=0, resolved_z=None,
                rejected=dict(invalid=0, height=0, zone=0, los=0, path=0, bounds=0, danger=0))


class RecoveryPlanningProtocolTests(unittest.TestCase):
    def test_older_recovery_payload_defaults_do_not_claim_work_or_terrain_success(self):
        parsed = ReturnRecovery.model_validate(legacy_recovery())
        for field in ("planning_deferred", "surface_corridor", "home_path_surface", "continuation_path_surface"):
            self.assertIs(getattr(parsed, field), False)
        for field in ("home_surface_failure", "continuation_surface_failure"):
            self.assertEqual(getattr(parsed, field), "NOT_CHECKED")

    def test_yield_and_each_route_source_survive_json_independently(self):
        payload = {**legacy_recovery(), "planning_deferred": True, "surface_corridor": True,
                   "home_path_surface": False, "home_surface_failure": "SURFACE_OBSTACLE",
                   "continuation_path_surface": True, "continuation_surface_failure": "NONE"}
        parsed = ReturnRecovery.model_validate_json(json.dumps(payload))
        again = ReturnRecovery.model_validate_json(parsed.model_dump_json())
        self.assertEqual(parsed, again)
        for key in payload:
            self.assertEqual(parsed.model_dump()[key], payload[key])
        self.assertEqual(parsed.home_path_failure, "NOT_CHECKED")
        self.assertEqual(parsed.continuation_path_failure, "NOT_CHECKED")
        self.assertEqual(parsed.rejected["path"], 0)

    def test_planning_booleans_reject_numeric_string_and_null_coercion(self):
        for field in ("planning_deferred", "surface_corridor", "home_path_surface", "continuation_path_surface"):
            for value in (0, 1, "true", "false", None, [], {}):
                with self.subTest(field=field, value=value), self.assertRaises(ValidationError):
                    ReturnRecovery.model_validate({**legacy_recovery(), field: value})

    def test_surface_failure_strings_remain_bounded_and_strict(self):
        for field in ("home_surface_failure", "continuation_surface_failure"):
            self.assertEqual(getattr(ReturnRecovery.model_validate({**legacy_recovery(), field: "x" * 100}), field), "x" * 100)
            for value in (1, True, None, "x" * 101, [], {}):
                with self.subTest(field=field, value=value), self.assertRaises(ValidationError):
                    ReturnRecovery.model_validate({**legacy_recovery(), field: value})

    def test_new_fields_do_not_relax_unknown_key_rejection(self):
        with self.assertRaises(ValidationError):
            ReturnRecovery.model_validate({**legacy_recovery(), "planning_deferred": True, "unknown_planning": True})


if __name__ == "__main__":
    unittest.main()
