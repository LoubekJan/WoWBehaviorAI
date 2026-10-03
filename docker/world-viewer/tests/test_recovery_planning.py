"""Recovery protocol regressions requiring only the production Pydantic model."""
import json
import unittest
from pathlib import Path

from pydantic import ValidationError

from app.telemetry import LivingPlanning, LivingRole, ReturnRecovery


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


class LivingPlanningProtocolTests(unittest.TestCase):
    def test_legacy_living_roles_default_to_unknown_work_without_return_recovery(self):
        batch = json.loads((Path(__file__).parent / "fixtures/telemetry_v2.json").read_text(encoding="utf-8"))
        source = next(a["living_role"] for a in batch["agents"] if a.get("living_role"))
        source.pop("return_recovery", None)
        parsed = LivingRole.model_validate(source)
        self.assertIsNone(parsed.return_recovery)
        self.assertEqual(parsed.planning.model_dump(), dict(deferred=False, reason="NONE", stage="NONE",
                         wait_ms=0, query_age_ms=0, no_progress_ms=0, resets=0))

    def test_admission_and_work_budget_survive_json_without_coercion(self):
        for reason, stage in (("ADMISSION", "DECISION"), ("WORK_BUDGET", "ADVICE"),
                              ("WORK_BUDGET", "RETURN"), ("WORK_BUDGET", "HUNT"), ("WORK_BUDGET", "FORAGE")):
            source = dict(deferred=True, reason=reason, stage=stage, wait_ms=43000,
                          query_age_ms=50000, no_progress_ms=45000, resets=3)
            parsed = LivingPlanning.model_validate_json(json.dumps(source))
            self.assertEqual(parsed.model_dump(), source)
            self.assertEqual(LivingPlanning.model_validate_json(parsed.model_dump_json()), parsed)

    def test_malformed_planning_cannot_claim_progress_or_yield(self):
        bad_fields = [("deferred", v) for v in (0, 1, "true", None)]
        bad_fields += [("reason", v) for v in ("UNKNOWN", 1, None)]
        bad_fields += [("stage", v) for v in ("UNKNOWN", 1, None)]
        for field in ("wait_ms", "query_age_ms", "no_progress_ms", "resets"):
            bad_fields += [(field, v) for v in (-1, True, "1", 1.5, None)]
        bad_fields += [("wait_ms", 18446744073709551616), ("resets", 4294967296), ("unknown", 1)]
        for field, value in bad_fields:
            with self.subTest(field=field, value=value), self.assertRaises(ValidationError):
                LivingPlanning.model_validate({field: value})

    def test_literal_uint_limits_roundtrip(self):
        parsed = LivingPlanning(wait_ms=18446744073709551615, resets=4294967295)
        self.assertEqual(LivingPlanning.model_validate_json(parsed.model_dump_json()), parsed)


if __name__ == "__main__":
    unittest.main()
