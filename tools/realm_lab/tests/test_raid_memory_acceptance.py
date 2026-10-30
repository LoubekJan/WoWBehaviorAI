"""Memory planning proof never substitutes danger labels for actual evidence."""
from copy import deepcopy
from functools import lru_cache
import unittest

from tools.realm_lab import raid_memory_acceptance as acceptance
from tools.realm_lab.tests import test_field_acceptance as fields

RAIDER, LEFT_GUARD = 901055, 901026


@lru_cache(maxsize=1)
def samples():
    """Synthetic only: source6 was interrupted on the LEFT approach."""
    rows = fields.happy_samples(end=300)
    camp = fields.DEPOTS[3]
    threat = dict(map_id=725, x=40, y=873, z=.15)
    encounter = dict(map_id=725, x=55, y=903, z=.15)
    for record in rows:
        t = record["elapsed_seconds"]
        raider, guard = fields.actor(record, RAIDER), fields.actor(record, LEFT_GUARD)
        plan = dict(reason="EXPLORE_UNKNOWN_STOCK" if t < 200 else "AVOID_REMEMBERED_DANGER",
            memory_records=int(t >= 51), route_id=1 if t < 200 else 2,
            known_stock=None, stock_age_ms=None, risk=0.0, retry_remaining_ms=0,
            danger=None if t < 51 else dict(position=deepcopy(threat), guard_spawn_id=str(LEFT_GUARD), age_ms=(t-50)*1000))
        raider["raid"].update(source_depot_id=6, attempt_id="1" if t < 200 else "2", planning=plan)
        source = next(actor["supply"]["depot"] for actor in record["state"]["agents"]
                      if (actor.get("supply") or {}).get("depot_id") == 6)
        raider["raid"]["source_depot"] = deepcopy(source)
        raider.update(in_combat=t == 50, action=None, action_source_goal=None,
                      action_started_at_ms=None, goal=None, goal_owner="")
        raider["living_role"].update(phase="IDLE", activity="NONE", started_at_ms=0)
        if t < 50:
            fields.move(raider, fields.blend(camp, encounter, t/50), moving=True)
            fields.owned(raider, "TO_SOURCE", "NONE", 0, moving=True, purpose="RAID_SOURCE")
            raider["raid"]["phase"] = "TO_SOURCE"
        elif t < 100:
            fields.move(raider, fields.blend(encounter, camp, (t-50)/50), moving=t > 50)
            raider["raid"]["phase"] = "FLEEING"
        elif t < 200:
            fields.move(raider, camp)
            raider["raid"]["phase"] = "COOLDOWN"
        else:
            fields.move(raider, dict(x=camp["x"]+(t-200), y=camp["y"], z=camp["z"]), moving=True)
            fields.owned(raider, "TO_SOURCE", "NONE", 200, moving=True, purpose="RAID_SOURCE")
            raider["raid"]["phase"] = "TO_SOURCE"
        guard.update(type="GUARD", alive=True, in_combat=t == 50,
                     target=dict(spawn_id=RAIDER) if t == 50 else None)
        guard["position"].update(threat, source="live")
    return rows


class RaidMemoryAcceptanceTests(unittest.TestCase):
    def test_legacy_recording_without_planning_remains_inconclusive(self):
        report = acceptance.evaluate_samples(fields.happy_samples())
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["planning_frames"], 0)
        self.assertEqual(report["violations"], [])

    def test_actual_guard_memory_links_to_nonlocal_owned_bypass_after_camp_return(self):
        report = acceptance.evaluate_samples(samples())
        self.assertEqual(report["status"], "PASS", report)
        receipt = report["decisions"][0]
        self.assertEqual((receipt["previous_source_depot_id"], receipt["source_depot_id"]), (6, 6))
        self.assertEqual((receipt["previous_route_id"], receipt["route_id"]), (1, 2))
        self.assertEqual(receipt["guard_spawn_id"], LEFT_GUARD)
        self.assertEqual(receipt["remembered_guard_position"]["y"], 873)
        self.assertEqual(receipt["decision"], "OWNED_DEPARTURE")
        self.assertIsNone(receipt["known_stock"])
        self.assertGreaterEqual(receipt["recalled_age_ms"], 120000)

    def test_guard_memory_cannot_move_the_left_guard_to_the_right_warehouse(self):
        rows = deepcopy(samples())
        for record in rows:
            danger = fields.actor(record, RAIDER)["raid"]["planning"]["danger"]
            if danger:
                danger["position"] = deepcopy(fields.DEPOTS[6])
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("raid_memory_guard_place_contradicts_observed_encounter", [v["code"] for v in report["violations"]])
        self.assertEqual(report["decisions"], [])

    def test_model_labels_or_memory_without_witnessed_attacker_do_not_prove_learning(self):
        rows = deepcopy(samples())
        for record in rows:
            fields.actor(record, LEFT_GUARD)["target"] = None
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["decisions"], [])
        rows = deepcopy(samples())
        for record in rows:
            if record["elapsed_seconds"] >= 200:
                fields.actor(record, RAIDER)["action"] = None
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_unchanged_route_and_target_or_recent_memory_do_not_prove_new_plan(self):
        rows = deepcopy(samples())
        for record in rows:
            fields.actor(record, RAIDER)["raid"]["planning"]["route_id"] = 1
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")
        rows = deepcopy(samples())
        for record in rows:
            danger = fields.actor(record, RAIDER)["raid"]["planning"]["danger"]
            if danger: danger["age_ms"] = 1000
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_unknown_stock_does_not_become_current_remote_ledger_knowledge(self):
        rows = deepcopy(samples())
        record = rows[250]
        fields.actor(record, RAIDER)["raid"]["planning"].update(known_stock=30, stock_age_ms=0)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("raid_stock_memory_without_matching_local_observation", [v["code"] for v in report["violations"]])

    def test_malformed_memory_cannot_become_a_qualifying_receipt(self):
        rows = deepcopy(samples())
        fields.actor(rows[250], RAIDER)["raid"]["planning"]["memory_records"] = True
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("invalid_raid_memory_planning", [v["code"] for v in report["violations"]])

    def test_fresh_snapshot_window_and_shared_food_conservation_remain_required(self):
        report = acceptance.evaluate_samples(samples()[:250])
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        rows = deepcopy(samples())
        fields.actor(rows[250], RAIDER)["supply"]["cargo"] = 1
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL", report)

    def test_actual_defer_at_camp_can_qualify_without_a_successful_theft(self):
        rows = deepcopy(samples())
        for record in rows:
            if record["elapsed_seconds"] >= 200:
                actor = fields.actor(record, RAIDER)
                fields.move(actor, fields.DEPOTS[3])
                actor.update(action=None, goal=None, action_source_goal=None, goal_owner="")
                actor["raid"].update(phase="WAITING_MEMORY", attempt_id="1")
                actor["raid"]["planning"].update(reason="DEFER_REMEMBERED_DANGER", route_id=1, retry_remaining_ms=10000)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["decisions"][0]["decision"], "DEFERRED")
        self.assertEqual(report["decisions"][0]["route_id"], 1)
        self.assertEqual(fields.actor(rows[-1], RAIDER)["supply"]["stolen"], "0")


if __name__ == "__main__":
    unittest.main()
