"""Read-only physical proof of a finite food raid and a prevented attempt.

The reviewed hunter and consumer gates remain required on the same 28-owner,
three-depot snapshot. Labels and cumulative counters alone are not proof.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
from typing import Iterable

try:
    from . import consumer_acceptance as consumers, raid_population as population
except ImportError:
    import consumer_acceptance as consumers
    import raid_population as population

supply = consumers.supply
RAIDER, GUARD = 901055, 901039
CONSUMERS = {**consumers.CONSUMERS, GUARD: dict(type="GUARD", role="GUARD", faction=1, depot=1)}
RAIDERS = {RAIDER: dict(type="COMBATANT", role="RAIDER", faction=2, depot=3, source_depot=1)}
RAID_COUNTERS = ("noticed", "interceptions", "thefts", "delivered", "prevented")


@dataclass
class RaidTrack:
    previous: dict | None = None
    attempt: dict | None = None
    work: dict | None = None
    successes: list = field(default_factory=list)
    prevented: list = field(default_factory=list)


class _Evaluator(consumers._Evaluator):
    def __init__(self, minimum_seconds=900, max_gap_seconds=5):
        super().__init__(minimum_seconds, max_gap_seconds, consumers=CONSUMERS, raiders=RAIDERS,
                         depots={3: population.DEPOT["position"]})
        self.raid_track = RaidTrack()
        self.previous_guard = None

    @staticmethod
    def owned_action(row, action, goal):
        actor, role = row["agent"], row["role"]
        return (actor.get("action") == action and actor.get("action_source_goal") == goal and
                actor.get("goal") == goal and actor.get("goal_owner") == "INDIVIDUAL" and
                supply.positive_int(actor.get("action_started_at_ms")) and
                actor["action_started_at_ms"] == role.get("started_at_ms"))

    @classmethod
    def owned_work(cls, row, phase):
        return (row["raid"]["phase"] == phase and row["role"].get("phase") == "ACTING" and
                row["role"].get("activity") == "WORK" and row["movement"].get("moving") is False and
                row["agent"].get("in_combat") is False and cls.owned_action(row, "AMBIENT", "LOCAL_ACTIVITY"))

    def row(self, actor, t, capture):
        row = super().row(actor, t, capture)
        sid = actor["spawn_id"]
        if row is None or sid not in (RAIDER, GUARD):
            return row
        if row["alive"] and sid == RAIDER:
            if row["role"].get("role") != "RAIDER":
                self.violation("raider_runtime_role_changed", sid)
                return None
            if not supply.finite(row["hunger"]) or not 0 <= row["hunger"] <= 1:
                self.quality["missing_raider_needs"] += 1
                return None
        raid = actor.get("raid")
        if not isinstance(raid, dict) or raid.get("enabled") is not True:
            self.quality["missing_raid_observation"] += 1
            return None
        expected_role, peer = ("RAIDER", GUARD) if sid == RAIDER else ("GUARD", RAIDER)
        counts = {key: supply.counter(raid.get(key)) for key in RAID_COUNTERS}
        attempt = supply.counter(raid.get("attempt_id"))
        if (raid.get("role") != expected_role or supply.counter(raid.get("target_spawn_id")) != peer or
            attempt is None or any(value is None for value in counts.values()) or
            not isinstance(raid.get("phase"), str) or not 0 < len(raid["phase"]) <= 80 or
            not isinstance(raid.get("outcome"), str) or len(raid["outcome"]) > 100 or
            (raid.get("source_depot_id"), raid.get("camp_depot_id")) != (1, 3)):
            self.violation("invalid_raid_capability_attempt_or_counter", sid)
            return None
        for key, depot_id in (("source_depot", 1), ("camp_depot", 3)):
            depot = raid.get(key)
            if not isinstance(depot, dict) or supply.point(depot.get("position")) is None:
                self.quality["missing_raid_depot"] += 1
                return None
            if (depot.get("id") != depot_id or depot["position"].get("map_id") != 725 or
                supply.dist3(supply.point(depot["position"]), supply.point(self.depots[depot_id])) > 0.05 or
                not supply.quantity(depot.get("stock")) or not supply.positive_int(depot.get("capacity")) or
                depot["stock"] > depot["capacity"] or
                any(supply.counter(depot.get(k, "0") if k == "stolen" else depot.get(k)) is None for k in ("delivered", "consumed", "stolen"))):
                self.violation("invalid_or_moved_raid_depot", sid)
                return None
        row.update(raid=raid, raid_counts=counts, attempt_id=attempt,
                   source_distance=supply.dist3(row["position"], supply.point(self.depots[1])),
                   camp_distance=supply.dist3(row["position"], supply.point(self.depots[3])))
        return row

    def update_consumers(self, rows):
        super().update_consumers(rows)
        # Compare all copies, including the source depot copied by the thief.
        depots = {row["depot"]["id"]: row["depot"] for row in rows.values() if "depot" in row}
        for sid in (RAIDER, GUARD):
            row = rows.get(sid)
            if row and "raid" in row:
                for key in ("source_depot", "camp_depot"):
                    depot = row["raid"][key]
                    if depot["id"] in depots and {**depots[depot["id"]], "stolen": depots[depot["id"]].get("stolen", "0")} != depot:
                        self.violation("conflicting_raid_and_supply_depot_copies", sid)
        self.update_raid(rows)

    def update_raid(self, rows):
        track = self.raid_track
        row, guard, old = rows.get(RAIDER), rows.get(GUARD), track.previous
        if row is None or "raid" not in row:
            track.previous = track.attempt = track.work = None
            self.previous_guard = guard
            return
        if old and (old["guid"], old["epoch"]) != (row["guid"], row["epoch"]):
            old = track.attempt = track.work = None
        if old and old["alive"] and row["alive"] and old["life"] != row["life"]:
            old = track.attempt = track.work = None
        if row["alive"] and row["attempt_id"] and row["raid"]["phase"] == "TO_SOURCE":
            if track.attempt is None or track.attempt["attempt_id"] != row["attempt_id"]:
                if row["cargo"] == 0 and self.owned_action(row, "MOVE_TO", "LOCAL_ACTIVITY"):
                    track.attempt = dict(epoch=self.epoch, attempt_id=row["attempt_id"], raider_spawn_id=RAIDER,
                        runtime_guid=row["guid"], lifetime_ms=row["life"], started_seconds=row["t"],
                        starting_stolen=row["counters"]["stolen"], starting_thefts=row["raid_counts"]["thefts"],
                        covered=["TO_SOURCE"], times={"TO_SOURCE": row["t"]}, carry_travel_yards=0)
        if old:
            if old["alive"] and row["alive"] and row["attempt_id"] < old["attempt_id"]:
                self.violation("raid_attempt_identity_reused_or_reset", RAIDER)
            delta = {key: row["counters"][key] - old["counters"][key] for key in (*supply.COUNTERS, "stolen")}
            event_delta = {key: row["raid_counts"][key] - old["raid_counts"][key] for key in RAID_COUNTERS}
            if any(value < 0 for value in (*delta.values(), *event_delta.values())):
                track.attempt = track.work = None
                self.quality["raid_counter_reset"] += 1
            else:
                if row["cargo"] - old["cargo"] != delta["stolen"] - delta["delivered"] - delta["lost"]:
                    self.violation("raid_cargo_changed_without_matching_transfer", RAIDER)
                if row["alive"] and old["alive"] and old["hunger"] - row["hunger"] > 0.000001 and not delta["consumed"]:
                    self.violation("raider_hunger_satisfied_without_food_debit", RAIDER)
                old_food, new_food = ((r["agent"].get("economy") or {}).get("food") for r in (old, row))
                if supply.quantity(old_food) and supply.quantity(new_food) and new_food > old_food:
                    self.violation("raider_created_personal_food", RAIDER)
                speed = max((v for v in (old["movement"].get("run_speed"), row["movement"].get("run_speed")) if supply.finite(v)), default=0)
                if speed and supply.dist3(old["position"], row["position"]) > speed * (row["t"] - old["t"]) + 3:
                    self.violation("unphysical_position_jump", RAIDER)
                self.transitions(track, old, row, guard, delta, event_delta)
        if self.owned_work(row, "STEALING") or self.owned_work(row, "DELIVERING"):
            attempt = track.work
            key = (row["attempt_id"], row["raid"]["phase"], row["role"]["started_at_ms"])
            if attempt is None or attempt["key"] != key:
                track.work = dict(key=key, first_capture=row["capture"])
        else:
            track.work = None
        track.previous, self.previous_guard = row, guard

    def transitions(self, track, old, row, guard, delta, event_delta):
        attempt = track.attempt
        for portions, depot_id, phase in ((delta["stolen"], 1, "STEALING"), (delta["delivered"], 3, "DELIVERING")):
            if portions:
                physical = (portions == 2 and old["attempt_id"] == row["attempt_id"] and self.owned_work(old, phase) and
                    old["source_distance" if depot_id == 1 else "camp_distance"] <= 3.01 and
                    row["source_distance" if depot_id == 1 else "camp_distance"] <= 3.01 and
                    row["movement"].get("moving") is False and row["agent"].get("in_combat") is False and
                    row["capture"] - old["role"]["started_at_ms"] >= 5000)
                if not physical:
                    self.violation("raid_transfer_without_owned_work_or_at_wrong_depot", RAIDER)
                    continue
                if phase == "STEALING" and (old["raid"]["source_depot"]["stock"] < 2 or event_delta["thefts"] != 1):
                    self.violation("raid_theft_without_available_food_or_matching_event", RAIDER)
                    continue
                if phase == "DELIVERING" and event_delta["delivered"] != 1:
                    self.violation("raid_delivery_without_matching_event", RAIDER)
                    continue
                if track.work is None or row["capture"] - track.work["first_capture"] < 5000:
                    self.quality["raid_work_start_not_observed"] += 1
                    continue
                if not attempt or attempt["attempt_id"] != row["attempt_id"]:
                    self.quality["raid_approach_not_observed"] += 1
                    continue
                if phase == "STEALING":
                    village_now = next((r for r in (guard, row) if r and r.get("depot", {}).get("id") == 1 and "depot_flow" in r), None)
                    if village_now is None or village_now["depot_flow"]["stolen"] < portions:
                        self.quality["raid_source_debit_not_reconciled"] += 1
                        continue
                    attempt.update(stolen_portions=portions, source_stock_before=old["raid"]["source_depot"]["stock"],
                        source_stock_after=row["raid"]["source_depot"]["stock"], theft_position=row["position"])
                    attempt["covered"].append("STOLEN"); attempt["times"]["STOLEN"] = row["t"]
                elif "STOLEN" in attempt["covered"] and "CARRIED" in attempt["covered"] and row.get("depot_flow", {}).get("delivered", 0) >= portions:
                    attempt["covered"].append("DELIVERED"); attempt["times"]["DELIVERED"] = row["t"]
                    attempt.update(delivered_portions=portions, camp_stock_before=old["raid"]["camp_depot"]["stock"],
                        camp_stock_after=row["raid"]["camp_depot"]["stock"], delivery_distance_yards=round(row["camp_distance"], 3))
                    track.successes.append(dict(attempt)); track.attempt = attempt = None
        if delta["consumed"]:
            if (delta["consumed"] != 1 or old["raid"]["phase"] != "EATING_CAMP" or
                not self.owned_action(old, "AMBIENT", "LOCAL_ACTIVITY") or old["role"].get("activity") != "EAT" or
                old["role"].get("phase") != "ACTING" or old["raid"]["camp_depot"]["stock"] < 1 or
                old["camp_distance"] > 3.01 or row["camp_distance"] > 3.01 or
                old["movement"].get("moving") is not False or row["movement"].get("moving") is not False or
                old["agent"].get("in_combat") is not False or row["agent"].get("in_combat") is not False or
                old["hunger"] - row["hunger"] < 0.1 or row.get("depot_flow", {}).get("consumed", 0) < 1 or
                row["capture"] - old["role"].get("started_at_ms", row["capture"]) < 5000):
                self.violation("raider_meal_without_owned_camp_food", RAIDER)
        if not attempt or attempt["attempt_id"] != row["attempt_id"]:
            return
        if "STOLEN" in attempt["covered"] and row["cargo"] == 2 and row["raid"]["phase"] == "TO_CAMP":
            attempt["carry_travel_yards"] = max(attempt["carry_travel_yards"], supply.distance(attempt["theft_position"], row["position"]))
            if attempt["carry_travel_yards"] >= 2 and self.owned_action(row, "MOVE_TO", "LOCAL_ACTIVITY") and "CARRIED" not in attempt["covered"]:
                attempt["covered"].append("CARRIED"); attempt["times"]["CARRIED"] = row["t"]
        previous_guard = self.previous_guard
        if guard and previous_guard and guard["alive"] and "raid" in guard and "raid" in previous_guard and guard["attempt_id"] == row["attempt_id"]:
            if (guard["guid"], guard["life"], guard["epoch"]) == (previous_guard["guid"], previous_guard["life"], previous_guard["epoch"]):
                if guard["raid_counts"]["noticed"] > previous_guard["raid_counts"]["noticed"] and "STOLEN" not in attempt["covered"]:
                    attempt["noticed_seconds"] = row["t"]
                    attempt["guard_runtime_guid"] = guard["guid"]; attempt["guard_lifetime_ms"] = guard["life"]
                if guard["raid_counts"]["interceptions"] > previous_guard["raid_counts"]["interceptions"]:
                    attempt["interception_count_observed"] = guard["raid_counts"]["interceptions"]
            if ("noticed_seconds" in attempt and "interception_count_observed" in attempt and "STOLEN" not in attempt["covered"] and
                guard["raid"]["phase"] == "INTERCEPTING" and guard["role"].get("phase") == "DEFENDING" and
                self.owned_action(guard, "ATTACK", "DEFEND") and (guard["agent"].get("target") or {}).get("spawn_id") == RAIDER and
                supply.dist3(guard["position"], row["position"]) <= 30.01):
                attempt["intercepted_seconds"] = row["t"]
        if event_delta["prevented"] > 0 and row["raid"]["outcome"] == "INTERCEPTED":
            attempt["prevented_count_observed"] = row["raid_counts"]["prevented"]
        if ("intercepted_seconds" in attempt and row["raid"]["outcome"] == "INTERCEPTED" and
            row["raid"]["phase"] in ("RETURNING_EMPTY", "COOLDOWN", "ACTOR_DEAD") and
            row["counters"]["stolen"] == attempt["starting_stolen"] and
            row["raid_counts"]["thefts"] == attempt["starting_thefts"] and row["cargo"] == 0 and
            "prevented_count_observed" in attempt and row["raid_counts"]["prevented"] >= attempt["prevented_count_observed"]):
            attempt["prevented_seconds"] = row["t"]
            track.prevented.append(dict(attempt)); track.attempt = None

    def report(self):
        report = super().report()
        epoch = report["selected_epoch"]
        successes = [r for r in self.raid_track.successes if r["epoch"] == epoch]
        prevented = [r for r in self.raid_track.prevented if r["epoch"] == epoch]
        missing = report["missing_evidence"][:]
        if not successes: missing.append("observed_owned_theft_carry_and_camp_delivery")
        if not prevented: missing.append("observed_guard_notice_interception_and_closed_attempt_without_theft")
        status = "FAIL" if report["violations"] else "INCONCLUSIVE" if missing else "PASS"
        report.update(profile="raids-v8", status=status, exit_code=supply.EXIT_CODES[status], missing_evidence=missing,
            raid_cycles=self.raid_track.successes, prevented_raids=self.raid_track.prevented,
            successful_raid_attempts=[r["attempt_id"] for r in successes], prevented_raid_attempts=[r["attempt_id"] for r in prevented])
        report["limitations"].append("Observed guard-target actions do not independently measure line of sight or attribute every damage event.")
        return report


def evaluate_samples(samples: Iterable[dict], *, minimum_seconds=900, max_gap_seconds=5):
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds)
    for sample in samples: evaluator.observe(sample)
    return evaluator.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=900)
    parser.add_argument("--max-gap-seconds", type=float, default=5)
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(supply.recording_rows(args.path), minimum_seconds=args.minimum_seconds, max_gap_seconds=args.max_gap_seconds)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile="raids-v8", status="INCONCLUSIVE", exit_code=2, missing_evidence=["readable_valid_recording"], input_error=type(error).__name__)
    rendered = json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2) + "\n"
    if args.output: args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
