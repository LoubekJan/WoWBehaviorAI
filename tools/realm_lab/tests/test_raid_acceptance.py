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
    return rows


class RaidAcceptanceTests(unittest.TestCase):
    def test_complete_theft_and_guard_prevention_preserve_all_existing_gates(self):
        report = acceptance.evaluate_samples(fixture())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual((report["expected_owned"], report["expected_native"]), (28, 72))
        self.assertEqual(report["successful_raid_attempts"], [1])
        self.assertEqual(report["prevented_raid_attempts"], [2])
        cycle = report["raid_cycles"][0]
        self.assertEqual(cycle["covered"], ["TO_SOURCE", "STOLEN", "CARRIED", "DELIVERED"])
        self.assertEqual(cycle["stolen_portions"], 2)
        self.assertEqual(cycle["delivered_portions"], 2)
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
                    elif missing == "work_start" and current["elapsed_seconds"] == 236: r["raid"]["phase"] = "WAITING"
                    elif missing == "notice": g["raid"]["noticed"] = "0"
                    elif missing == "intercept" and g["raid"]["phase"] == "INTERCEPTING": g["raid"]["phase"] = "GUARD_POST"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_free_food_foreign_depot_unowned_work_and_impossible_cargo_fail(self):
        for change in ("cargo", "free_hunger", "away", "action_start", "source_stock", "native_guard"):
            with self.subTest(change=change):
                rows = deepcopy(fixture())
                r = actor(rows[241], RAIDER)
                if change == "cargo": r["supply"]["cargo"] = 3
                elif change == "free_hunger": r["needs"]["hunger"] = 0
                elif change == "away": move(r, CAMP)
                elif change == "action_start": actor(rows[240], RAIDER)["action_started_at_ms"] -= 1000
                elif change == "source_stock": r["raid"]["source_depot"]["stock"] += 2
                else: actor(rows[483], GUARD)["control_mode"] = "OBSERVE_ONLY"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_no_stitching_theft_to_delivery_across_stale_or_new_incarnation(self):
        for change in ("stale", "guid", "life", "attempt"):
            rows = deepcopy(fixture())
            if change == "stale": rows[270]["status"] = "stale"
            else:
                for current in rows[270:]:
                    r = actor(current, RAIDER)
                    if change == "guid": r["runtime_guid"] += "-new"
                    elif change == "life": r["living_role"]["lifetime_ms"] = BASE + 269000
                    elif int(r["raid"]["attempt_id"]) > 0: r["raid"]["attempt_id"] = str(int(r["raid"]["attempt_id"])+1)
            self.assertNotEqual(acceptance.evaluate_samples(rows)["status"], "PASS")


if __name__ == "__main__":
    unittest.main()
