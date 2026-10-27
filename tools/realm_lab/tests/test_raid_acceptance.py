"""Raid proof requires both conserved physical theft and an observed defence."""
from copy import deepcopy
from functools import lru_cache
import unittest

from tools.realm_lab import raid_acceptance as acceptance, consumer_acceptance
from tools.realm_lab.tests.test_consumer_acceptance import happy_samples as consumer_samples
from tools.realm_lab.tests.test_supply_acceptance import ACTORS, BASE, DEPOTS, actor, interpolate, move, position

RAIDER, GUARD = acceptance.RAIDER, acceptance.GUARD
SOURCE, CAMP = DEPOTS[1], acceptance.population.DEPOT["position"]


def owned(current, action, phase, started, *, activity="NONE", goal="LOCAL_ACTIVITY", purpose="NONE"):
    current.update(action=action, action_source_goal=goal, action_started_at_ms=BASE + started * 1000,
                   goal=goal, goal_owner="INDIVIDUAL")
    current["living_role"].update(phase=phase, activity=activity, started_at_ms=BASE + started * 1000,
                                  movement_purpose=purpose)


@lru_cache(maxsize=1)
def fixture():
    rows = consumer_samples()
    for t in range(len(rows), 491):
        current = deepcopy(rows[-1])
        current.update(sequence=t + 1, elapsed_seconds=t)
        current["state"]["captured_at_ms"] = BASE + t * 1000
        rows.append(current)
    for current in rows:
        t = current["elapsed_seconds"]
        raider, guard = actor(current, RAIDER), actor(current, GUARD)
        stolen, delivered = 2 if t >= 241 else 0, 2 if t >= 333 else 0
        for sid, a, role, kind, faction, depot_id in ((RAIDER, raider, "RAIDER", "COMBATANT", 2, 3),
                                                     (GUARD, guard, "GUARD", "GUARD", 1, 1)):
            a.update(native_creature=False, type=kind, control_mode="AI_WORLD_CONTROLLED", world_faction=faction,
                needs=dict(hunger=0.3), economy=dict(food=0, resource=0, money="0"),
                living_role=dict(enabled=True, role=role, phase="IDLE", activity="NONE", started_at_ms=0,
                                 lifetime_ms=BASE - 1000, movement_purpose="NONE"))
            a["supply"] = dict(enabled=True, role="RAIDER" if sid == RAIDER else "CONSUMER", phase="ORDINARY",
                last_result="READY", depot_id=depot_id, cargo=stolen-delivered if sid == RAIDER else 0,
                capacity=2 if sid == RAIDER else 0, harvested="0", delivered=str(delivered if sid == RAIDER else 0),
                consumed="0", lost="0", stolen=str(stolen if sid == RAIDER else 0),
                carcass_remaining=None, carcass_spawn_id=None, carcass_life=None, hunting_ground=None)
            a["raid"] = dict(enabled=True, role=role, phase="COOLDOWN" if sid == RAIDER else "GUARD_HOME", outcome="READY",
                attempt_id="0", target_spawn_id=str(GUARD if sid == RAIDER else RAIDER),
                noticed="1" if sid == GUARD and t >= 483 else "0", interceptions="1" if sid == GUARD and t >= 483 else "0",
                thefts="1" if sid == RAIDER and t >= 241 else "0", delivered="1" if sid == RAIDER and t >= 333 else "0",
                prevented="1" if sid == RAIDER and t >= 484 else "0", source_depot_id=1, camp_depot_id=3)
        if 150 <= t < 236 or 400 <= t < 484:
            start = 150 if t < 400 else 400
            move(raider, interpolate(CAMP, SOURCE, (t-start)/85), True)
            owned(raider, "MOVE_TO", "MOVING", start, purpose="RAID_SOURCE")
            raider["raid"].update(phase="TO_SOURCE", attempt_id="1" if t < 400 else "2")
        elif 236 <= t < 241:
            move(raider, SOURCE)
            owned(raider, "AMBIENT", "ACTING", 236, activity="WORK", purpose="RAID_THEFT")
            raider["raid"].update(phase="STEALING", attempt_id="1")
        elif t == 241:
            move(raider, SOURCE)
            raider["living_role"]["started_at_ms"] = BASE + 236000
            raider["raid"].update(phase="TO_CAMP", attempt_id="1", outcome="STOLEN")
        elif 242 <= t < 328:
            move(raider, interpolate(SOURCE, CAMP, (t-242)/85), t < 327)
            owned(raider, "MOVE_TO", "MOVING", 242, purpose="RAID_CAMP")
            raider["raid"].update(phase="TO_CAMP", attempt_id="1", outcome="STOLEN")
        elif 328 <= t < 333:
            move(raider, CAMP)
            owned(raider, "AMBIENT", "ACTING", 328, activity="WORK", purpose="RAID_DELIVERY")
            raider["raid"].update(phase="DELIVERING", attempt_id="1", outcome="STOLEN")
        elif 333 <= t < 400:
            move(raider, CAMP)
            raider["living_role"]["started_at_ms"] = BASE + 328000
            raider["raid"].update(phase="COOLDOWN", attempt_id="1", outcome="DELIVERED")
        elif t >= 484:
            at = interpolate(CAMP, SOURCE, 83/85)
            move(raider, at)
            raider["raid"].update(phase="FLEEING" if t == 484 else "RETURNING_EMPTY", attempt_id="2", outcome="INTERCEPTED")
            if t == 484:
                owned(raider, "FLEE", "FLEEING", 484, goal="FLEE_DANGER")
                raider["in_combat"] = True
        if 460 <= t <= 480:
            move(guard, interpolate(ACTORS[GUARD]["home"], SOURCE, (t-460)/20), t < 480)
            owned(guard, "MOVE_TO", "MOVING", 460, purpose="GUARD_POST")
            guard["raid"]["phase"] = "GUARD_POST"
        elif t > 480:
            move(guard, SOURCE)
            guard["raid"].update(phase="GUARD_POST", attempt_id="2" if t >= 483 else "0")
        if 483 <= t <= 486:
            owned(guard, "ATTACK", "DEFENDING", 483, goal="DEFEND")
            guard.update(in_combat=True, target=dict(spawn_id=RAIDER, position=position(raider["position"])))
            guard["raid"]["phase"] = "INTERCEPTING"
        # The fixture starts from a known six-portion village ledger balance;
        # every later portion is either an observed hunt or an actual transfer.
        for a in current["state"]["agents"]:
            s = a.get("supply")
            if not s or a["spawn_id"] in (RAIDER, GUARD): continue
            s["stolen"] = "0"
            if s.get("depot_id") == 1:
                s["depot"]["stock"] += 6 - stolen
            if s.get("depot"):
                s["depot"]["stolen"] = str(stolen if s["depot_id"] == 1 else 0)
            s["accounting"].update(initial_food="6", in_depots=str(int(s["accounting"]["in_depots"])+6-stolen+delivered),
                                     in_cargo=str(int(s["accounting"]["in_cargo"])+stolen-delivered))
        source = deepcopy(actor(current, 901034)["supply"]["depot"])
        camp = dict(id=3, name="Defias camp", position=deepcopy(CAMP), stock=delivered, capacity=30,
                    low_threshold=4, delivered=str(delivered), consumed="0", stolen="0")
        balance = deepcopy(actor(current, 901034)["supply"]["accounting"])
        for a in (raider, guard):
            a["supply"].update(depot=deepcopy(camp if a["spawn_id"] == RAIDER else source), accounting=deepcopy(balance))
            a["raid"].update(source_depot=deepcopy(source), camp_depot=deepcopy(camp))
    # The real observer captures owned short actions every 250 ms. Keep the
    # long approach at one second while retaining every action-local capture.
    dense = []
    for current in rows:
        dense.append(current)
        t = current["elapsed_seconds"]
        if 236 <= t < 241 or 328 <= t < 333:
            for fraction in (.25, .5, .75):
                extra = deepcopy(current)
                extra["elapsed_seconds"] = t + fraction
                extra["state"]["captured_at_ms"] = BASE + int((t + fraction) * 1000)
                dense.append(extra)
    return resequence(dense)


def resequence(rows):
    for sequence, current in enumerate(rows, 1):
        current["sequence"] = sequence
    return rows


def at(rows, elapsed):
    return next(current for current in rows if current["elapsed_seconds"] == elapsed)


class RaidAcceptanceTests(unittest.TestCase):
    def test_complete_theft_and_guard_prevention_preserve_all_existing_gates(self):
        report = acceptance.evaluate_samples(fixture())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual((report["expected_owned"], report["expected_native"]), (28, 72))
        self.assertEqual(report["successful_raid_attempts"], [1])
        self.assertEqual(report["prevented_raid_attempts"], [2])
        prevented = report["prevented_raids"][0]
        self.assertEqual([prevented[key] for key in ("noticed_seconds", "interception_event_seconds",
            "intercepted_seconds", "prevention_event_seconds", "prevented_seconds")], [483, 483, 483, 484, 485])
        cycle = report["raid_cycles"][0]
        self.assertEqual(cycle["covered"], ["TO_SOURCE", "STOLEN", "CARRIED", "DELIVERED"])
        self.assertEqual(cycle["stolen_portions"], 2)
        self.assertEqual(cycle["delivered_portions"], 2)
        for key in ("theft_work", "delivery_work"):
            self.assertEqual(cycle[key]["start_observation_lag_ms"], 0)
            self.assertEqual(cycle[key]["observed_camera_span_ms"], 5000)
            self.assertEqual(cycle[key]["receipt_capture_age_ms"], 5000)
            self.assertEqual(cycle[key]["max_sample_gap_ms"], 250)
            self.assertEqual(cycle[key]["unique_samples"], 21)
        self.assertGreater(cycle["carry_travel_yards"], 600)
        self.assertEqual(len(report["consumer_cycles"]), 3)
        self.assertEqual(len(report["cycles"]), 3)

    def test_existing_consumer_gate_rejects_unreviewed_raid_ownership(self):
        self.assertEqual(consumer_acceptance.evaluate_samples(fixture())["status"], "FAIL")

    def test_missing_carry_work_notice_or_intercept_never_passes(self):
        for missing in ("carry", "work_start", "notice", "intercept"):
            with self.subTest(missing=missing):
                rows = deepcopy(fixture())
                for current in rows:
                    r, g = actor(current, RAIDER), actor(current, GUARD)
                    if missing == "carry" and r["raid"]["phase"] == "TO_CAMP": r["raid"]["phase"] = "WAITING"
                    elif missing == "work_start" and 236 <= current["elapsed_seconds"] <= 236.5: r["raid"]["phase"] = "WAITING"
                    elif missing == "notice": g["raid"]["noticed"] = "0"
                    elif missing == "intercept" and g["raid"]["phase"] == "INTERCEPTING": g["raid"]["phase"] = "GUARD_POST"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_free_food_foreign_depot_unowned_work_and_impossible_cargo_fail(self):
        for change in ("cargo", "free_hunger", "away", "action_start", "source_stock", "native_guard"):
            with self.subTest(change=change):
                rows = deepcopy(fixture())
                r = actor(at(rows, 241), RAIDER)
                if change == "cargo": r["supply"]["cargo"] = 3
                elif change == "free_hunger": r["needs"]["hunger"] = 0
                elif change == "away": move(r, CAMP)
                elif change == "action_start": actor(at(rows, 240.75), RAIDER)["action_started_at_ms"] -= 1000
                elif change == "source_stock": r["raid"]["source_depot"]["stock"] += 2
                else: actor(at(rows, 483), GUARD)["control_mode"] = "OBSERVE_ONLY"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_no_stitching_theft_to_delivery_across_stale_or_new_incarnation(self):
        for change in ("stale", "guid", "life", "attempt"):
            rows = deepcopy(fixture())
            if change == "stale": at(rows, 270)["status"] = "stale"
            else:
                for current in rows:
                    if current["elapsed_seconds"] < 270:
                        continue
                    r = actor(current, RAIDER)
                    if change == "guid": r["runtime_guid"] += "-new"
                    elif change == "life": r["living_role"]["lifetime_ms"] = BASE + 269000
                    elif int(r["raid"]["attempt_id"]) > 0: r["raid"]["attempt_id"] = str(int(r["raid"]["attempt_id"])+1)
            self.assertNotEqual(acceptance.evaluate_samples(rows)["status"], "PASS")

    def test_bounded_start_lag_proves_work_without_claiming_five_seconds_of_camera_span(self):
        rows = resequence([deepcopy(current) for current in fixture()
                           if current["elapsed_seconds"] not in (236, 328)])
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        for key in ("theft_work", "delivery_work"):
            evidence = report["raid_cycles"][0][key]
            self.assertEqual(evidence["start_observation_lag_ms"], 250)
            self.assertEqual(evidence["observed_camera_span_ms"], 4750)
            self.assertEqual(evidence["receipt_capture_age_ms"], 5000)
            self.assertEqual(evidence["max_sample_gap_ms"], 250)
            self.assertEqual(evidence["unique_samples"], 20)
        self.assertTrue(any("upper bound" in limitation for limitation in report["limitations"]))

    def test_late_first_owned_capture_cannot_prove_work(self):
        rows = resequence([deepcopy(current) for current in fixture()
                           if current["elapsed_seconds"] not in (236, 236.25, 236.5)])
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertFalse(report["raid_cycles"])

    def test_start_lag_and_capture_gap_limits_are_inclusive(self):
        rows = resequence([deepcopy(current) for current in fixture()
                           if current["elapsed_seconds"] not in (236, 236.25, 238.25, 238.5)])
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        evidence = report["raid_cycles"][0]["theft_work"]
        self.assertEqual(evidence["start_observation_lag_ms"], 500)
        self.assertEqual(evidence["observed_camera_span_ms"], 4500)
        self.assertEqual(evidence["max_sample_gap_ms"], 750)

    def test_receipt_capture_age_below_five_seconds_fails(self):
        rows = resequence([deepcopy(current) for current in fixture() if current["elapsed_seconds"] != 236])
        for current in rows:
            if 236.25 <= current["elapsed_seconds"] <= 241:
                r = actor(current, RAIDER)
                r["living_role"]["started_at_ms"] += 250
                if current["elapsed_seconds"] < 241:
                    r["action_started_at_ms"] += 250
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL", report)
        self.assertFalse(report["raid_cycles"])

    def test_intervening_invalid_work_cannot_rearm_the_same_action_stamp(self):
        for change in ("started", "moving", "depot", "combat", "goal", "action"):
            with self.subTest(change=change):
                rows = deepcopy(fixture())
                r = actor(at(rows, 238.5), RAIDER)
                if change == "started":
                    r["living_role"]["started_at_ms"] += 1
                    r["action_started_at_ms"] += 1
                elif change == "moving": r["movement"]["moving"] = True
                elif change == "depot": r["position"]["x"] += 4
                elif change == "combat": r["in_combat"] = True
                elif change == "goal": r["goal"] = "DEFEND"
                else: r["action"] = "MOVE_TO"
                report = acceptance.evaluate_samples(rows)
                self.assertEqual(report["status"], "INCONCLUSIVE", report)
                self.assertFalse(report["raid_cycles"])

    def test_action_local_gap_cannot_hide_in_a_valid_global_epoch(self):
        rows = resequence([deepcopy(current) for current in fixture()
                           if current["elapsed_seconds"] not in (238.25, 238.5, 238.75)])
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["selected_epoch"], 0)
        self.assertFalse(report["raid_cycles"])

    def test_identical_capture_replay_is_not_an_additional_owned_work_sample(self):
        rows = deepcopy(fixture())
        index = rows.index(at(rows, 238.5))
        replay = deepcopy(rows[index])
        replay["elapsed_seconds"] += .05
        rows.insert(index + 1, replay)
        report = acceptance.evaluate_samples(resequence(rows))
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["raid_cycles"][0]["theft_work"]["unique_samples"], 21)
        self.assertEqual(report["quality"]["identical_capture_ignored"], 1)

    def test_old_capture_replay_breaks_the_continuous_work_proof(self):
        rows = deepcopy(fixture())
        index = rows.index(at(rows, 238.5))
        replay = deepcopy(at(rows, 237.5))
        replay["elapsed_seconds"] = 238.55
        rows.insert(index + 1, replay)
        self.assertNotEqual(acceptance.evaluate_samples(resequence(rows))["status"], "PASS")

    def test_guard_notice_cannot_be_stitched_to_a_new_incarnation_or_missing_observation(self):
        for change in ("guid", "life", "missing"):
            with self.subTest(change=change):
                rows = deepcopy(fixture())
                for current in rows:
                    t = current["elapsed_seconds"]
                    g = actor(current, GUARD)
                    if t < 484:
                        continue
                    if change == "guid": g["runtime_guid"] += "-new"
                    elif change == "life": g["living_role"]["lifetime_ms"] = BASE + 483500
                    elif t == 484: g.pop("raid")
                report = acceptance.evaluate_samples(rows)
                self.assertNotEqual(report["status"], "PASS", report)
                self.assertFalse(report["prevented_raids"])

    def test_guard_notice_attack_and_prevention_must_be_observed_in_order(self):
        for change in ("late_notice", "late_attack", "early_prevention", "early_interception_event"):
            with self.subTest(change=change):
                rows = deepcopy(fixture())
                for current in rows:
                    t = current["elapsed_seconds"]
                    r, g = actor(current, RAIDER), actor(current, GUARD)
                    if change == "late_notice" and t < 485:
                        g["raid"]["noticed"] = "0"
                    elif change == "late_attack" and 483 <= t < 485:
                        owned(g, "AMBIENT", "ACTING", 483, activity="LOOK")
                    elif change == "early_prevention" and t >= 482:
                        r["raid"]["prevented"] = "1"
                        if t == 482: r["raid"]["outcome"] = "INTERCEPTED"
                    elif change == "early_interception_event" and 482 <= t < 483:
                        g["raid"].update(interceptions="1", attempt_id="2")
                report = acceptance.evaluate_samples(rows)
                self.assertNotEqual(report["status"], "PASS", report)
                self.assertFalse(report["prevented_raids"])

    def test_known_guard_corpse_can_close_an_already_observed_interception(self):
        rows = deepcopy(fixture())
        for current in rows:
            if current["elapsed_seconds"] < 484:
                continue
            g = actor(current, GUARD)
            g.update(alive=False, health=0, in_combat=False, action="NONE", target=None)
            g["living_role"].update(phase="IDLE", activity="NONE")
            g["raid"]["phase"] = "ACTOR_DEAD"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["prevented_raid_attempts"], [2])


if __name__ == "__main__":
    unittest.main()
