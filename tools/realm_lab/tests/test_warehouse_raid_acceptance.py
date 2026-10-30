"""All-warehouse theft proof preserves ownership, food flow and physical receipts."""
from copy import deepcopy
from functools import lru_cache
import math
import unittest

from tools.realm_lab import field_acceptance, warehouse_raid_acceptance as acceptance
from tools.realm_lab.tests import test_field_acceptance as fields

RAIDER, GUARD = acceptance.raids.RAIDER, acceptance.raids.GUARD


def move(actor, target, moving=False):
    actor["position"].update(target)
    actor["movement"].update(moving=moving, run_speed=14.0,
                            home_distance=math.dist(fields.xyz(actor["position"]), fields.xyz(actor["home"])))


def owned(actor, phase, started, *, work=False):
    actor.update(action="AMBIENT" if work else "MOVE_TO", action_source_goal="LOCAL_ACTIVITY",
        goal="LOCAL_ACTIVITY", goal_owner="INDIVIDUAL", action_started_at_ms=fields.BASE+started*1000)
    actor["living_role"].update(phase="ACTING" if work else "MOVING", activity="WORK" if work else "NONE",
        started_at_ms=fields.BASE+started*1000, movement_purpose="RAID_THEFT" if phase == "STEALING" else "RAID_DELIVERY" if phase == "DELIVERING" else "RAID_SOURCE" if phase == "TO_SOURCE" else "RAID_CAMP")
    actor["raid"]["phase"] = phase


@lru_cache(maxsize=4)
def fixture(source_id):
    """Synthetic only: a known opening balance never proves a harvest."""
    rows = fields.happy_samples(end=400)
    source, camp = fields.DEPOTS[source_id], fields.DEPOTS[3]
    for record in rows:
        t = record["elapsed_seconds"]
        thief = fields.actor(record, RAIDER)
        fields.actor(record, GUARD)["raid"].update(phase="GUARD_HOME", outcome="READY", attempt_id="0",
            target_spawn_id=str(RAIDER), noticed="0", interceptions="0", thefts="0", delivered="0", prevented="0")
        stolen, delivered = 2*int(t >= 256), 2*int(t >= 363)
        for actor in record["state"]["agents"]:
            supplied = actor.get("supply")
            if not supplied:
                continue
            accounting = supplied["accounting"]
            accounting.update(initial_food="6", in_depots=str(int(accounting["in_depots"])+6-stolen+delivered),
                              in_cargo=str(int(accounting["in_cargo"])+stolen-delivered))
            if supplied["depot_id"] == source_id:
                supplied["depot"]["stock"] += 6-stolen
                supplied["depot"]["stolen"] = str(stolen)
            if supplied["depot_id"] == 3:
                supplied["depot"].update(stock=delivered, delivered=str(delivered))
        thief["supply"].update(cargo=stolen-delivered, stolen=str(stolen), delivered=str(delivered))
        thief["raid"].update(source_depot_id=source_id, attempt_id="1" if t >= 150 else "0",
            phase="COOLDOWN", outcome="DELIVERED" if delivered else "STOLEN" if stolen else "READY",
            target_spawn_id=str(GUARD), noticed="0", interceptions="0", prevented="0",
            thefts=str(int(bool(stolen))), delivered=str(int(bool(delivered))))
        move(thief, camp)
        thief.update(action=None, action_source_goal=None, action_started_at_ms=None, goal=None, goal_owner="")
        thief["living_role"].update(phase="IDLE", activity="NONE", started_at_ms=0)
        if 150 <= t <= 250:
            move(thief, fields.blend(camp, source, (t-150)/100), t < 250)
            owned(thief, "TO_SOURCE", 150)
        elif 251 <= t < 256:
            move(thief, source); owned(thief, "STEALING", 251, work=True)
        elif t == 256:
            move(thief, source)
            thief["living_role"]["started_at_ms"] = fields.BASE+251000
            thief["raid"]["phase"] = "TO_CAMP"
        elif 257 <= t <= 357:
            move(thief, fields.blend(source, camp, (t-257)/100), t < 357)
            owned(thief, "TO_CAMP", 257)
        elif 358 <= t < 363:
            move(thief, camp); owned(thief, "DELIVERING", 358, work=True)
        elif t >= 363:
            thief["living_role"]["started_at_ms"] = fields.BASE+358000
        # Supply and raid snapshots are copies of the same producer ledger.
        depots = {actor["supply"]["depot_id"]: actor["supply"]["depot"] for actor in record["state"]["agents"] if actor.get("supply")}
        for actor in record["state"]["agents"]:
            if actor.get("raid"):
                rid = source_id if actor["spawn_id"] == RAIDER else 1
                actor["raid"].update(source_depot=deepcopy(depots[rid]), camp_depot=deepcopy(depots[3]))
    dense = []
    for record in rows:
        dense.append(record)
        t = record["elapsed_seconds"]
        if 251 <= t < 256 or 358 <= t < 363:
            for fraction in (.25, .5, .75):
                extra = deepcopy(record)
                extra["elapsed_seconds"] = t+fraction
                extra["state"]["captured_at_ms"] = fields.BASE+int((t+fraction)*1000)
                dense.append(extra)
    for sequence, record in enumerate(dense, 1):
        record["sequence"] = sequence
    return dense


class WarehouseRaidAcceptanceTests(unittest.TestCase):
    def test_each_source_has_physical_theft_carry_delivery_and_source_specific_debit(self):
        for source_id in (1, 4, 5, 6):
            with self.subTest(source_id=source_id):
                report = acceptance.evaluate_samples(fixture(source_id), required_sources=(source_id,))
                self.assertEqual(report["status"], "PASS", report)
                self.assertEqual(report["completed_source_depot_ids"], [source_id])
                receipt = report["raid_cycles"][0]
                self.assertEqual(receipt["source_depot_id"], source_id)
                self.assertEqual(receipt["covered"], ["TO_SOURCE", "STOLEN", "CARRIED", "DELIVERED"])
                self.assertEqual(receipt["theft_work"]["receipt_capture_age_ms"], 5000)
                self.assertEqual(receipt["delivery_work"]["receipt_capture_age_ms"], 5000)
                field_report = field_acceptance.evaluate_samples(fixture(source_id))
                self.assertEqual(field_report["status"], "PASS", field_report)

    def test_all_sources_required_by_default_and_starting_balances_never_prove_theft(self):
        report = acceptance.evaluate_samples(fixture(5))
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["unobserved_source_depot_ids"], [1, 4, 6])
        report = acceptance.evaluate_samples([row for row in fixture(5) if row["elapsed_seconds"] < 150],
                                             required_sources=(5,), minimum_seconds=100)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertFalse(report["raid_cycles"])
        for sources in ((), (2,), (3,), (True,), (4, 4)):
            with self.subTest(sources=sources), self.assertRaises(ValueError):
                acceptance.evaluate_samples([], required_sources=sources)

    def test_missing_work_start_cannot_be_joined_to_late_receipt(self):
        rows = deepcopy(fixture(4))
        for record in rows:
            if 251 <= record["elapsed_seconds"] <= 251.5:
                fields.actor(record, RAIDER)["raid"]["phase"] = "WAITING"
        report = acceptance.evaluate_samples(rows, required_sources=(4,))
        self.assertNotEqual(report["status"], "PASS", report)
        self.assertFalse(report["raid_cycles"])

    def test_source_cannot_change_within_attempt_or_transfer_debit_to_another_village(self):
        rows = deepcopy(fixture(5))
        for record in rows:
            if record["elapsed_seconds"] < 256:
                continue
            thief = fields.actor(record, RAIDER)
            thief["raid"].update(source_depot_id=4,
                source_depot=deepcopy(fields.actor(record, 901016)["supply"]["depot"]))
        report = acceptance.evaluate_samples(rows, required_sources=(5,))
        self.assertEqual(report["status"], "FAIL", report)
        codes = {v["code"] for v in report["violations"]}
        self.assertIn("raid_source_changed_within_attempt", codes)
        self.assertIn("depot_stock_change_not_reconciled", codes)

    def test_field_warehouse_work_requires_the_tighter_runtime_anchor(self):
        rows = deepcopy(fixture(4))
        target = dict(fields.DEPOTS[4])
        target["x"] += 2.5
        for record in rows:
            if 251 <= record["elapsed_seconds"] <= 256:
                move(fields.actor(record, RAIDER), target)
        report = acceptance.evaluate_samples(rows, required_sources=(4,))
        self.assertEqual(report["status"], "FAIL", report)
        self.assertFalse(report["raid_cycles"])
        self.assertIn("raid_transfer_without_owned_work_or_at_wrong_depot", {v["code"] for v in report["violations"]})

    def test_wrong_position_and_disagreeing_warehouse_copies_are_rejected(self):
        rows = deepcopy(fixture(6))
        for record in rows:
            if 251 <= record["elapsed_seconds"] <= 256:
                move(fields.actor(record, RAIDER), fields.DEPOTS[5])
        report = acceptance.evaluate_samples(rows, required_sources=(6,))
        self.assertEqual(report["status"], "FAIL", report)
        self.assertFalse(report["raid_cycles"])
        rows = deepcopy(fixture(4))
        fields.actor(rows[-1], RAIDER)["raid"]["source_depot"]["stock"] += 1
        report = acceptance.evaluate_samples(rows, required_sources=(4,))
        self.assertEqual(report["status"], "FAIL", report)
        self.assertIn("conflicting_depot_copies", {v["code"] for v in report["violations"]})


if __name__ == "__main__":
    unittest.main()
