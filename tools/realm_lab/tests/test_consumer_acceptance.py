"""Physical consumer journeys share the finite food ledger with hunter proof."""
from contextlib import redirect_stdout
from copy import deepcopy
import gzip
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import consumer_acceptance as acceptance, supply_acceptance
from tools.realm_lab.tests.test_supply_acceptance import (
    ACTORS, BASE, DEPOTS, actor, happy_samples as hunter_samples, interpolate, move,
)


def happy_samples():
    rows = hunter_samples()
    for t in range(len(rows), 145):
        row = deepcopy(rows[-1])
        row.update(sequence=t + 1, elapsed_seconds=t)
        row["state"]["captured_at_ms"] = BASE + t * 1000
        rows.append(row)
    for row in rows:
        t = row["elapsed_seconds"]
        for sid, spec in acceptance.CONSUMERS.items():
            current = actor(row, sid)
            hunter = actor(row, 901034 if spec["depot"] == 1 else 901061)
            current.update(native_creature=False, type=spec["type"], control_mode="AI_WORLD_CONTROLLED",
                world_faction=spec["faction"], needs=dict(hunger=0.8 if t < 111 else 0),
                economy=dict(food=0, resource=0, money="0"))
            current["living_role"] = dict(enabled=True, role=spec["role"], phase="IDLE", activity="NONE",
                started_at_ms=0, lifetime_ms=BASE - 1000, movement_purpose="NONE")
            current["supply"] = dict(enabled=True, role="CONSUMER", phase="NO_FOOD" if t < 94 else "ORDINARY",
                depot_id=spec["depot"], cargo=0, capacity=0, harvested="0", delivered="0",
                consumed="1" if t >= 111 else "0", lost="0", last_result="NO_FOOD" if t < 94 else "READY",
                carcass_remaining=None, carcass_spawn_id=None, carcass_life=None, hunting_ground=None,
                depot=deepcopy(hunter["supply"]["depot"]), accounting=deepcopy(hunter["supply"]["accounting"]))
            if 96 <= t <= 105:
                move(current, interpolate(ACTORS[sid]["home"], DEPOTS[spec["depot"]], (t - 96) / 9), t < 105)
                current["supply"]["phase"] = "TO_DEPOT"
                current.update(action="MOVE_TO", action_source_goal="LOCAL_ACTIVITY")
                current["living_role"].update(phase="MOVING", movement_purpose="GET_DEPOT_FOOD")
            elif 106 <= t <= 110:
                move(current, DEPOTS[spec["depot"]])
                current["supply"]["phase"] = "EATING_DEPOT"
                current.update(action="AMBIENT", action_source_goal="LOCAL_ACTIVITY", goal="LOCAL_ACTIVITY",
                               goal_owner="INDIVIDUAL", action_started_at_ms=BASE + 106000)
                current["living_role"].update(phase="ACTING", activity="EAT", started_at_ms=BASE + 106000)
            elif t == 111:
                move(current, DEPOTS[spec["depot"]])
                current["supply"].update(phase="WAITING", last_result="ATE_DEPOT")
            elif 112 <= t <= 121:
                move(current, interpolate(DEPOTS[spec["depot"]], ACTORS[sid]["home"], (t - 112) / 9), t < 121)
                current["supply"]["phase"] = "RETURNING_HOME"
                current.update(action="MOVE_TO", action_source_goal="LOCAL_ACTIVITY")
                current["living_role"].update(phase="MOVING", movement_purpose="RETURN_CONSUMER_HOME")
        if t >= 111:
            for current in row["state"]["agents"]:
                supplied = current.get("supply")
                if supplied is None:
                    continue
                supplied["accounting"]["in_depots"] = str(int(supplied["accounting"]["in_depots"]) - 3)
                supplied["accounting"]["consumed"] = str(int(supplied["accounting"]["consumed"]) + 3)
                if supplied.get("depot"):
                    count = 2 if supplied["depot_id"] == 1 else 1
                    supplied["depot"]["stock"] -= count
                    supplied["depot"]["consumed"] = str(int(supplied["depot"]["consumed"]) + count)
    return rows


class ConsumerAcceptanceTests(unittest.TestCase):
    def test_three_meals_returns_and_existing_hunter_chains_share_an_epoch(self):
        report = acceptance.evaluate_samples(happy_samples())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["expected_owned"], 26)
        self.assertEqual(report["expected_native"], 74)
        self.assertEqual(report["completed_consumers"], sorted(acceptance.CONSUMER_IDS))
        self.assertEqual(len(report["consumer_cycles"]), 3)
        self.assertEqual(len(report["cycles"]), 3)
        for cycle in report["consumer_cycles"]:
            self.assertGreater(cycle["travel_yards"], 2)
            self.assertEqual(cycle["meal"]["portions"], 1)
            self.assertGreaterEqual(cycle["meal"]["observed_meal_seconds"], 5)
            self.assertLessEqual(cycle["home_distance_yards"], 3)
        self.assertTrue(all(report["empty_depot_observations"].values()))

    def test_old_supply_gate_stays_strict_about_its_23_owner_roster(self):
        self.assertEqual(supply_acceptance.evaluate_samples(hunter_samples())["status"], "PASS")
        self.assertEqual(supply_acceptance.evaluate_samples(happy_samples())["status"], "FAIL")

    def test_ownership_role_capability_free_food_and_away_debits_fail(self):
        for change in ("ownership", "role", "cargo", "hunting", "free_hunger", "personal_food", "away", "foreign_depot"):
            with self.subTest(change=change):
                rows = happy_samples()
                current = actor(rows[111], 901035)
                if change == "ownership": current["control_mode"] = "OBSERVE_ONLY"
                elif change == "role": current["living_role"]["role"] = "HUNTER"
                elif change == "cargo": current["supply"].update(cargo=1, capacity=1)
                elif change == "hunting": current["supply"]["harvested"] = "1"
                elif change == "free_hunger": actor(rows[10], 901035)["needs"]["hunger"] = 0
                elif change == "personal_food": actor(rows[10], 901035)["economy"]["food"] = 6
                elif change == "away": move(current, ACTORS[901035]["home"])
                else: current["supply"]["depot_id"] = 2
                report = acceptance.evaluate_samples(rows)
                self.assertEqual(report["status"], "FAIL", report)

    def test_missing_journey_meal_start_or_return_never_passes(self):
        for change in ("journey", "meal_start", "return", "guid", "life"):
            with self.subTest(change=change):
                rows = happy_samples()
                for row in rows:
                    current = actor(row, 901035)
                    if change == "journey" and current["supply"]["phase"] == "TO_DEPOT":
                        current["supply"]["phase"] = "ORDINARY"
                    elif change == "return" and current["supply"]["phase"] == "RETURNING_HOME":
                        current["supply"]["phase"] = "ORDINARY"
                    elif change == "meal_start" and row["elapsed_seconds"] == 106:
                        current["supply"]["phase"] = "WAITING"
                    elif change == "guid": current.pop("runtime_guid")
                    elif change == "life": current["living_role"].pop("lifetime_ms")
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_stale_gap_or_new_incarnation_does_not_stitch_a_consumer_journey(self):
        for change in ("stale", "life", "gap"):
            rows = happy_samples()
            if change == "stale": rows[107]["status"] = "stale"
            elif change == "gap": rows.pop(107)
            else:
                for row in rows[108:]: actor(row, 901035)["living_role"]["lifetime_ms"] = BASE + 108000
            report = acceptance.evaluate_samples(rows)
            self.assertNotEqual(report["status"], "PASS", report)
            self.assertFalse(report["violations"], report)

    def test_empty_stock_does_not_prove_or_allow_a_meal(self):
        rows = happy_samples()[:20]
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertFalse(report["consumer_meals"])
        actor(rows[10], 901035)["needs"]["hunger"] = 0
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_concurrent_depot_debits_need_one_reconciled_finite_stock(self):
        rows = happy_samples()
        # Two villagers claim food, but only one debit is exported by the depot.
        for row in rows[111:]:
            for current in row["state"]["agents"]:
                supplied = current.get("supply")
                if supplied and supplied.get("depot_id") == 1:
                    supplied["depot"]["stock"] += 1
                    supplied["depot"]["consumed"] = str(int(supplied["depot"]["consumed"]) - 1)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("depot_stock_change_not_reconciled", {v["code"] for v in report["violations"]})

    def test_meal_action_start_and_individual_goal_must_match_the_observed_role(self):
        for field, value in (("action_started_at_ms", BASE + 105000), ("goal", "REST"),
                              ("goal_owner", "GROUP"), ("action_source_goal", "HUNT")):
            with self.subTest(field=field):
                rows = happy_samples()
                actor(rows[110], 901035)[field] = value
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")
        rows = happy_samples()
        for row in rows: actor(row, 901035).pop("action_started_at_ms", None)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertFalse(report["violations"])

    def test_recording_cli_replays_without_altering_source(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3]) as directory:
            source, output = Path(directory) / "part.jsonl.gz", Path(directory) / "result.json"
            with gzip.open(source, "wt", encoding="utf-8") as stream:
                for row in happy_samples(): stream.write(json.dumps(row) + "\n")
            before = source.read_bytes()
            with redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([str(source), "--output", str(output)]), 0)
            self.assertEqual(json.loads(output.read_text())["status"], "PASS")
            self.assertEqual(source.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
