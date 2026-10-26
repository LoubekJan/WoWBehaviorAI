"""Read-only acceptance of physical consumer meals in the consumers-v8 world.

The unchanged hunter gate must pass in the same ledger epoch. Each reviewed
consumer must walk to its own depot, finish a continuously observed meal with
a real debit, and physically return home to its ordinary role.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
from typing import Iterable

try:
    from . import supply_acceptance as supply, consumer_population as population
except ImportError:
    import supply_acceptance as supply
    import consumer_population as population

CONSUMERS = {sid: dict(type={0: "CIVILIAN", 5: "COMBATANT"}[population.CONSUMER_TYPES[sid]],
    role=population.CONSUMER_ROLES[sid], faction=population.CONSUMER_FACTIONS[sid],
    depot=population.CONSUMER_DEPOTS[sid]) for sid in population.CONSUMER_IDS}
CONSUMER_IDS = frozenset(CONSUMERS)


@dataclass
class ConsumerTrack:
    previous: dict | None = None
    journey: dict | None = None
    meal_attempt: dict | None = None
    pending_return: dict | None = None
    cycles: list = field(default_factory=list)
    meals: list = field(default_factory=list)
    empty_observations: int = 0


class _Evaluator(supply._Evaluator):
    def __init__(self, minimum_seconds=900, max_gap_seconds=5):
        super().__init__(minimum_seconds, max_gap_seconds, consumers=CONSUMERS)
        self.consumer_tracks = {sid: ConsumerTrack() for sid in CONSUMERS}

    @staticmethod
    def meal_ownership(row):
        actor, started = row["agent"], row["role"].get("started_at_ms")
        if any(actor.get(key) is None for key in ("goal", "goal_owner", "action_started_at_ms")):
            return None
        return (actor["goal"] == "LOCAL_ACTIVITY" and actor["goal_owner"] == "INDIVIDUAL" and
                supply.positive_int(actor["action_started_at_ms"]) and actor["action_started_at_ms"] == started)

    def row(self, actor, t, capture):
        row = super().row(actor, t, capture)
        sid = actor["spawn_id"]
        if row is None or sid not in CONSUMERS or not row["alive"]:
            return row
        needs, role, movement = actor.get("needs") or {}, row["role"], row["movement"]
        if role.get("role") is not None and role["role"] != CONSUMERS[sid]["role"]:
            self.violation("consumer_ordinary_role_changed", sid)
            return None
        if (role.get("role") != CONSUMERS[sid]["role"] or not supply.finite(needs.get("hunger")) or
            not 0 <= needs["hunger"] <= 1 or not supply.finite(movement.get("home_distance"))):
            self.quality["missing_consumer_role_or_needs"] += 1
            return None
        if abs(movement["home_distance"] - supply.distance(row["position"], supply.point(actor["home"]))) > 1:
            self.violation("runtime_home_changed", sid)
            return None
        if row["phase"] == "EATING_DEPOT" and self.meal_ownership(row) is False:
            self.violation("consumer_meal_action_or_goal_ownership_changed", sid)
        return row

    def update_consumers(self, rows):
        for sid, track in self.consumer_tracks.items():
            row = rows.get(sid)
            old = track.previous
            if row is None or not row["alive"]:
                track.previous = track.journey = track.meal_attempt = track.pending_return = None
                continue
            if old and (old["guid"], old["life"], old["epoch"]) != (row["guid"], row["life"], row["epoch"]):
                old = track.journey = track.meal_attempt = track.pending_return = None
            if old:
                delta = {key: row["counters"][key] - old["counters"][key] for key in supply.COUNTERS}
                dt = row["t"] - old["t"]
                speeds = [v for v in (old["movement"].get("run_speed"), row["movement"].get("run_speed")) if supply.finite(v) and v > 0]
                step = supply.dist3(old["position"], row["position"])
                if speeds and step > max(speeds) * dt + 3:
                    self.violation("unphysical_position_jump", sid)
                if any(value < 0 for value in delta.values()):
                    old = track.journey = track.meal_attempt = track.pending_return = None
                    self.quality["consumer_counter_reset"] += 1
                else:
                    if old["hunger"] - row["hunger"] > 0.000001 and delta["consumed"] == 0:
                        self.violation("hunger_satisfied_without_food_debit", sid)
                    old_food = (old["agent"].get("economy") or {}).get("food")
                    new_food = (row["agent"].get("economy") or {}).get("food")
                    if supply.quantity(old_food) and supply.quantity(new_food) and new_food > old_food:
                        self.violation("consumer_created_personal_food", sid)
                    if delta["consumed"]:
                        self.finish_meal(sid, track, old, row, delta["consumed"])
                    if track.journey and (old["movement"].get("moving") is True or row["movement"].get("moving") is True):
                        track.journey["travel_yards"] += step
            outgoing = (row["phase"] == "TO_DEPOT" and row["role"].get("phase") == "MOVING" and
                        row["role"].get("movement_purpose") == "GET_DEPOT_FOOD" and
                        row["agent"].get("action") == "MOVE_TO" and row["agent"].get("action_source_goal") == "LOCAL_ACTIVITY")
            if outgoing and track.journey is None and row["depot_distance"] > 3.01:
                track.journey = dict(epoch=self.epoch, consumer_spawn_id=sid, runtime_guid=row["guid"],
                    lifetime_ms=row["life"], depot_id=CONSUMERS[sid]["depot"], departure_seconds=row["t"],
                    departure_depot_distance_yards=round(row["depot_distance"], 3), travel_yards=0)
            eating = (self.activity(row, "EATING_DEPOT", "EAT") and row["depot_distance"] <= 3.01 and
                      supply.positive_int(row["role"].get("started_at_ms")) and self.meal_ownership(row) is True)
            if eating:
                started = row["role"]["started_at_ms"]
                if track.meal_attempt is None or track.meal_attempt["started"] != started:
                    track.meal_attempt = dict(started=started, first_capture=row["capture"])
            else:
                track.meal_attempt = None
            if row["phase"] == "NO_FOOD" and row["depot"]["stock"] == 0:
                track.empty_observations += 1
            if track.pending_return:
                if (row["phase"] == "RETURNING_HOME" and row["role"].get("movement_purpose") == "RETURN_CONSUMER_HOME" and
                    row["movement"].get("moving") is True):
                    track.pending_return["return_movement_observed"] = True
                if (row["phase"] == "ORDINARY" and row["movement"].get("home_distance", 999) <= 3.01 and
                    row["role"].get("role") == CONSUMERS[sid]["role"] and
                    track.pending_return.get("return_movement_observed")):
                    track.pending_return["returned_seconds"] = row["t"]
                    track.pending_return["home_distance_yards"] = round(row["movement"]["home_distance"], 3)
                    track.cycles.append(track.pending_return)
                    track.pending_return = track.journey = None
            track.previous = row

    def finish_meal(self, sid, track, old, row, portions):
        attempt = track.meal_attempt
        physically_owned = (self.activity(old, "EATING_DEPOT", "EAT") and
            old["depot_distance"] <= 3.01 and row["depot_distance"] <= 3.01 and
            row["movement"].get("moving") is False and row["agent"].get("in_combat") is False and
            supply.positive_int(old["role"].get("started_at_ms")) and
            row["capture"] - old["role"]["started_at_ms"] >= 5000 and
            old["depot"]["stock"] > 0 and portions == 1 and
            old["hunger"] - row["hunger"] >= 0.1)
        if not physically_owned:
            self.violation("consumer_meal_without_owned_action_stock_or_hunger_relief", sid)
            return
        ownership = self.meal_ownership(old)
        if ownership is not True:
            if ownership is False:
                self.violation("consumer_meal_action_or_goal_ownership_changed", sid)
            else:
                self.quality["missing_consumer_meal_action_or_goal_ownership"] += 1
            return
        if "depot_flow" not in row:
            self.quality["consumer_meal_depot_participant_not_observed"] += 1
            return
        if row["depot_flow"]["consumed"] < portions:
            self.violation("consumer_meal_without_reconciled_depot_debit", sid)
            return
        if attempt is None or row["capture"] - attempt["first_capture"] < 5000:
            self.quality["consumer_meal_start_not_observed"] += 1
            return
        meal = dict(epoch=self.epoch, consumer_spawn_id=sid, depot_id=CONSUMERS[sid]["depot"],
            seconds=row["t"], portions=portions, observed_meal_seconds=(row["capture"] - attempt["first_capture"]) / 1000,
            hunger_before=old["hunger"], hunger_after=row["hunger"], stock_before=old["depot"]["stock"],
            stock_after=row["depot"]["stock"], distance_yards=round(row["depot_distance"], 3),
            aggregate_consumed_during_sample=row["depot_flow"]["consumed"],
            aggregate_delivered_during_sample=row["depot_flow"]["delivered"])
        track.meals.append(meal)
        if track.journey and track.journey["travel_yards"] >= 2:
            track.pending_return = dict(**track.journey, meal=meal, return_movement_observed=False)

    def report(self):
        report = super().report()
        cycles = [cycle for track in self.consumer_tracks.values() for cycle in track.cycles]
        meals = [meal for track in self.consumer_tracks.values() for meal in track.meals]
        # Positive proof must share the hunter gate's uninterrupted ledger epoch.
        epoch = report["selected_epoch"]
        completed = {cycle["consumer_spawn_id"] for cycle in cycles if cycle["epoch"] == epoch}
        missing = report["missing_evidence"] + [f"physical_depot_meal_and_return_to_ordinary_role_{sid}"
            for sid in sorted(CONSUMER_IDS - completed)]
        status = "FAIL" if report["violations"] else "INCONCLUSIVE" if missing else "PASS"
        report.update(profile="consumers-v8", status=status, exit_code=supply.EXIT_CODES[status],
            missing_evidence=missing, consumer_cycles=cycles, consumer_meals=meals,
            completed_consumers=sorted(completed),
            empty_depot_observations={str(sid): track.empty_observations for sid, track in self.consumer_tracks.items()})
        report["limitations"].extend([
            "Position/action samples do not independently measure line of sight; the runtime checks it before debit.",
            "Observed individual goal ownership and matching action/role start times do not independently expose the internal goal-start timestamp.",
            "A last-portion race or an empty-depot refusal is not proven unless that situation is actually observed; ledger unit tests cover the race."])
        return report


def evaluate_samples(samples: Iterable[dict], *, minimum_seconds=900, max_gap_seconds=5) -> dict:
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds)
    for sample in samples:
        evaluator.observe(sample)
    return evaluator.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="recorder directory, .jsonl or .jsonl.gz")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=900)
    parser.add_argument("--max-gap-seconds", type=float, default=5)
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(supply.recording_rows(args.path), minimum_seconds=args.minimum_seconds,
                                  max_gap_seconds=args.max_gap_seconds)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile="consumers-v8", status="INCONCLUSIVE", exit_code=2,
                      missing_evidence=["readable_valid_recording"], input_error=type(error).__name__)
    rendered = json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
