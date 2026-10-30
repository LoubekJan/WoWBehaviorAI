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
WORK_FIRST_CAPTURE_MAX_LAG_MS = 500
WORK_CAPTURE_MAX_GAP_MS = 750
WORK_MINIMUM_AGE_MS = 5000


@dataclass
class RaidTrack:
    previous: dict | None = None
    attempt: dict | None = None
    work: dict | None = None
    blocked_work: set = field(default_factory=set)
    successes: list = field(default_factory=list)
    prevented: list = field(default_factory=list)


class _Evaluator(consumers._Evaluator):
    def __init__(self, minimum_seconds=900, max_gap_seconds=5):
        super().__init__(minimum_seconds, max_gap_seconds, consumers=CONSUMERS, raiders=RAIDERS,
                         depots={3: population.DEPOT["position"]})
        self.raid_source_depot_ids = frozenset((1,))
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

    @staticmethod
    def work_key(row, phase):
        started, action_started = row["role"].get("started_at_ms"), row["agent"].get("action_started_at_ms")
        return (row["guid"], row["life"], row["epoch"], row["attempt_id"], phase,
                started if supply.positive_int(started) else None,
                action_started if supply.positive_int(action_started) else None)

    @staticmethod
    def break_work(track):
        if track.work is not None:
            track.blocked_work.add(track.work["key"])
        track.work = None

    def work_radius(self, row, phase):
        return 3.01

    def observe_work(self, track, row):
        phase = row["raid"]["phase"]
        if phase not in ("STEALING", "DELIVERING"):
            self.break_work(track)
            return
        key = self.work_key(row, phase)
        if track.work is not None and track.work["key"] != key:
            self.break_work(track)
        distance_key = "source_distance" if phase == "STEALING" else "camp_distance"
        started = row["role"].get("started_at_ms")
        valid = (row["alive"] and self.owned_work(row, phase) and row[distance_key] <= self.work_radius(row, phase) and
                 supply.positive_int(started) and started >= row["life"])
        if not valid:
            track.blocked_work.add(key)
            self.break_work(track)
            return
        if key in track.blocked_work:
            return
        if track.work is None:
            lag = row["capture"] - started
            if not 0 <= lag <= WORK_FIRST_CAPTURE_MAX_LAG_MS:
                track.blocked_work.add(key)
                return
            track.work = dict(key=key, started=started, first_capture=row["capture"],
                last_capture=row["capture"], start_observation_lag_ms=lag, max_sample_gap_ms=0, unique_samples=1)
            return
        gap = row["capture"] - track.work["last_capture"]
        if not 0 < gap <= WORK_CAPTURE_MAX_GAP_MS:
            self.break_work(track)
            return
        track.work["last_capture"] = row["capture"]
        track.work["max_sample_gap_ms"] = max(track.work["max_sample_gap_ms"], gap)
        track.work["unique_samples"] += 1

    def work_receipt_evidence(self, track, old, row, phase):
        work = track.work
        if (work is None or work["key"] != self.work_key(old, phase) or
            work["last_capture"] != old["capture"] or
            (old["guid"], old["life"], old["epoch"], old["attempt_id"]) !=
                (row["guid"], row["life"], row["epoch"], row["attempt_id"]) or
            row["role"].get("started_at_ms") != work["started"]):
            self.quality["raid_work_continuity_not_observed"] += 1
            return None
        gap = row["capture"] - work["last_capture"]
        age = row["capture"] - work["started"]
        if not 0 < gap <= WORK_CAPTURE_MAX_GAP_MS or age < WORK_MINIMUM_AGE_MS:
            self.quality["raid_work_receipt_gap_or_age_invalid"] += 1
            return None
        # The camera span includes the final receipt frame, whose action has
        # already ended; unique_samples also includes that receipt. Age at
        # capture bounds the receipt observation; only
        # the reviewed runtime enforces the exact instant of the ledger debit.
        return dict(owned_start_ms=work["started"], first_owned_capture_ms=work["first_capture"],
            last_owned_capture_ms=work["last_capture"], receipt_capture_ms=row["capture"],
            start_observation_lag_ms=work["start_observation_lag_ms"],
            observed_camera_span_ms=row["capture"] - work["first_capture"], receipt_capture_age_ms=age,
            max_sample_gap_ms=max(work["max_sample_gap_ms"], gap), unique_samples=work["unique_samples"] + 1)

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
        source_id = raid.get("source_depot_id")
        allowed_sources = self.raid_source_depot_ids if sid == RAIDER else (1,)
        if (raid.get("role") != expected_role or supply.counter(raid.get("target_spawn_id")) != peer or
            attempt is None or any(value is None for value in counts.values()) or
            not isinstance(raid.get("phase"), str) or not 0 < len(raid["phase"]) <= 80 or
            not isinstance(raid.get("outcome"), str) or len(raid["outcome"]) > 100 or
            type(source_id) is not int or source_id not in allowed_sources or raid.get("camp_depot_id") != 3):
            self.violation("invalid_raid_capability_attempt_or_counter", sid)
            return None
        for key, depot_id in (("source_depot", source_id), ("camp_depot", 3)):
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
                   source_distance=supply.dist3(row["position"], supply.point(self.depots[source_id])),
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
            self.break_work(track)
            track.previous = track.attempt = track.work = None
            self.previous_guard = guard
            return
        if old and (old["guid"], old["epoch"]) != (row["guid"], row["epoch"]):
            old = track.attempt = track.work = None
            track.blocked_work.clear()
        if old and old["alive"] and row["alive"] and old["life"] != row["life"]:
            old = track.attempt = track.work = None
            track.blocked_work.clear()
        if row["alive"] and row["attempt_id"] and row["raid"]["phase"] == "TO_SOURCE":
            if track.attempt is None or track.attempt["attempt_id"] != row["attempt_id"]:
                if row["cargo"] == 0 and self.owned_action(row, "MOVE_TO", "LOCAL_ACTIVITY"):
                    track.attempt = dict(epoch=self.epoch, attempt_id=row["attempt_id"], raider_spawn_id=RAIDER,
                        runtime_guid=row["guid"], lifetime_ms=row["life"], source_depot_id=row["raid"]["source_depot_id"], started_seconds=row["t"],
                        starting_stolen=row["counters"]["stolen"], starting_thefts=row["raid_counts"]["thefts"],
                        covered=["TO_SOURCE"], times={"TO_SOURCE": row["t"]}, carry_travel_yards=0)
        if old:
            if (old["attempt_id"] == row["attempt_id"] and row["attempt_id"] and
                old["raid"]["source_depot_id"] != row["raid"]["source_depot_id"]):
                self.violation("raid_source_changed_within_attempt", RAIDER)
                track.attempt = track.work = None
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
        self.observe_work(track, row)
        track.previous, self.previous_guard = row, guard

    def transitions(self, track, old, row, guard, delta, event_delta):
        attempt = track.attempt
        for portions, depot_id, phase in ((delta["stolen"], row["raid"]["source_depot_id"], "STEALING"), (delta["delivered"], 3, "DELIVERING")):
            if portions:
                physical = (portions == 2 and old["attempt_id"] == row["attempt_id"] and self.owned_work(old, phase) and
                    old["source_distance" if phase == "STEALING" else "camp_distance"] <= self.work_radius(old, phase) and
                    row["source_distance" if phase == "STEALING" else "camp_distance"] <= self.work_radius(row, phase) and
                    row["movement"].get("moving") is False and row["agent"].get("in_combat") is False and
                    row["capture"] - old["role"]["started_at_ms"] >= WORK_MINIMUM_AGE_MS)
                if not physical:
                    self.violation("raid_transfer_without_owned_work_or_at_wrong_depot", RAIDER)
                    continue
                if phase == "STEALING" and (old["raid"]["source_depot"]["stock"] < 2 or event_delta["thefts"] != 1):
                    self.violation("raid_theft_without_available_food_or_matching_event", RAIDER)
                    continue
                if phase == "DELIVERING" and event_delta["delivered"] != 1:
                    self.violation("raid_delivery_without_matching_event", RAIDER)
                    continue
                work_evidence = self.work_receipt_evidence(track, old, row, phase)
                if work_evidence is None:
                    continue
                if not attempt or attempt["attempt_id"] != row["attempt_id"]:
                    self.quality["raid_approach_not_observed"] += 1
                    continue
                if phase == "STEALING":
                    village_now = next((r for r in (guard, row) if r and r.get("depot", {}).get("id") == depot_id and "depot_flow" in r), None)
                    source_flow = row.get("source_depot_flow") or (village_now or {}).get("depot_flow")
                    if source_flow is None or source_flow["stolen"] < portions:
                        self.quality["raid_source_debit_not_reconciled"] += 1
                        continue
                    attempt.update(stolen_portions=portions, source_stock_before=old["raid"]["source_depot"]["stock"],
                        source_stock_after=row["raid"]["source_depot"]["stock"], theft_position=row["position"],
                        theft_work=work_evidence)
                    attempt["covered"].append("STOLEN"); attempt["times"]["STOLEN"] = row["t"]
                elif "STOLEN" in attempt["covered"] and "CARRIED" in attempt["covered"] and row.get("depot_flow", {}).get("delivered", 0) >= portions:
                    attempt["covered"].append("DELIVERED"); attempt["times"]["DELIVERED"] = row["t"]
                    attempt.update(delivered_portions=portions, camp_stock_before=old["raid"]["camp_depot"]["stock"],
                        camp_stock_after=row["raid"]["camp_depot"]["stock"], delivery_distance_yards=round(row["camp_distance"], 3),
                        delivery_work=work_evidence)
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
        same_guard = (guard and previous_guard and "raid" in guard and "raid" in previous_guard and
            (guard["guid"], guard["life"], guard["epoch"]) ==
                (previous_guard["guid"], previous_guard["life"], previous_guard["epoch"]))
        # Freeze the detecting incarnation. Losing even one usable observation
        # cannot join its notice to a later respawn's attack or terminal receipt.
        # A known corpse with that same identity may close an already witnessed
        # attack; it need not still be alive or own ATTACK at the thief's death.
        if "noticed_seconds" in attempt:
            bound_guard = (same_guard and guard["guid"] == attempt["guard_runtime_guid"] and
                guard["life"] == attempt["guard_lifetime_ms"] and guard["epoch"] == attempt["epoch"] and
                guard["attempt_id"] == row["attempt_id"] and
                all(guard["raid_counts"][key] >= previous_guard["raid_counts"][key] for key in ("noticed", "interceptions")))
            if not bound_guard and not attempt.get("guard_proof_broken"):
                attempt["guard_proof_broken"] = True
                self.quality["guard_interception_observation_discontinuous"] += 1
        if (same_guard and guard["alive"] and guard["attempt_id"] == row["attempt_id"] and
            not attempt.get("guard_proof_broken") and "STOLEN" not in attempt["covered"]):
            if guard["raid_counts"]["noticed"] > previous_guard["raid_counts"]["noticed"]:
                attempt.setdefault("noticed_seconds", row["t"])
                attempt.setdefault("guard_runtime_guid", guard["guid"])
                attempt.setdefault("guard_lifetime_ms", guard["life"])
            if ("noticed_seconds" in attempt and row["t"] >= attempt["noticed_seconds"] and
                guard["raid_counts"]["interceptions"] > previous_guard["raid_counts"]["interceptions"]):
                attempt["interception_count_observed"] = guard["raid_counts"]["interceptions"]
                attempt.setdefault("interception_event_seconds", row["t"])
            if ("interception_event_seconds" in attempt and row["t"] >= attempt["interception_event_seconds"] and
                guard["raid"]["phase"] == "INTERCEPTING" and guard["role"].get("phase") == "DEFENDING" and
                self.owned_action(guard, "ATTACK", "DEFEND") and (guard["agent"].get("target") or {}).get("spawn_id") == RAIDER and
                supply.dist3(guard["position"], row["position"]) <= 30.01):
                attempt.setdefault("intercepted_seconds", row["t"])
        if event_delta["prevented"] > 0 and row["raid"]["outcome"] == "INTERCEPTED":
            if (not attempt.get("guard_proof_broken") and "intercepted_seconds" in attempt and
                attempt["noticed_seconds"] <= attempt["intercepted_seconds"] <= row["t"]):
                attempt["prevented_count_observed"] = row["raid_counts"]["prevented"]
                attempt.setdefault("prevention_event_seconds", row["t"])
            else:
                attempt["guard_proof_broken"] = True
                self.quality["guard_prevention_before_observed_interception"] += 1
        if (not attempt.get("guard_proof_broken") and "prevention_event_seconds" in attempt and
            attempt["noticed_seconds"] <= attempt["intercepted_seconds"] <= attempt["prevention_event_seconds"] <= row["t"] and
            row["raid"]["outcome"] == "INTERCEPTED" and
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
        report["limitations"].append("Work uses bounded sampling; the reviewed runtime enforces the exact commit time. Receipt capture age is an upper bound, not an independent debit timestamp.")
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
