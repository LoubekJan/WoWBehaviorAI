"""Read-only evidence of recalled raid experience and a later executed plan.

The unchanged field gate checks all forty identities and shared food flows.
A PASS here is not a successful theft: it links an observed guard attack to
old, nonlocal danger memory and a changed owned departure or a deferred raid.
Counterfactual scoring correctness belongs to the pure planner tests.
"""
from __future__ import annotations

import argparse
from collections import deque
import json
from pathlib import Path
from typing import Iterable

try:
    from . import field_acceptance as fields
except ImportError:
    import field_acceptance as fields

RAIDER = 901055
SOURCES = frozenset((1, 4, 5, 6))
MIN_RECALL_AGE_MS = 120000
OBSERVATION_TIME_TOLERANCE_MS = 1000
OBSERVATION_POINT_TOLERANCE_YARDS = 10
NONLOCAL_YARDS = 60
DEFER_REASONS = frozenset(("WAIT_DANGER", "ALL_ROUTES_DANGEROUS", "DEFER_REMEMBERED_DANGER"))


def integer(value, maximum=2**64-1):
    return type(value) is int and 0 <= value <= maximum


class _Evaluator:
    def __init__(self, minimum_seconds=300, max_gap_seconds=2):
        self.field_gate = fields._Evaluator(minimum_seconds, max_gap_seconds)
        self.minimum_seconds, self.max_gap = minimum_seconds, max_gap_seconds
        self.events = deque(maxlen=1024)
        self.stock_sights = deque(maxlen=1024)
        self.returned = {}
        self.decisions, self.closed = [], set()
        self.planning_frames = 0

    def violation(self, code):
        self.field_gate.violation(code, RAIDER)

    def plan(self, raid):
        plan = raid.get("planning")
        if plan is None:
            return None
        if (not isinstance(plan, dict) or not isinstance(plan.get("reason"), str) or
            not 0 < len(plan["reason"]) <= 80 or not integer(plan.get("memory_records"), 128) or
            not integer(plan.get("route_id"), 2) or not fields.finite(plan.get("risk")) or
            isinstance(plan["risk"], bool) or not 0 <= plan["risk"] <= 20 or not integer(plan.get("retry_remaining_ms"))):
            self.violation("invalid_raid_memory_planning")
            return None
        stock, age = plan.get("known_stock"), plan.get("stock_age_ms")
        if (stock is None) != (age is None) or stock is not None and (not integer(stock, 30) or not integer(age)):
            self.violation("invalid_raid_stock_memory")
            return None
        danger = plan.get("danger")
        if danger is not None and (not isinstance(danger, dict) or fields.point(danger.get("position")) is None or
            danger["position"].get("map_id") != 725 or fields.counter(danger.get("guard_spawn_id")) is None or
            not integer(danger.get("age_ms"))):
            self.violation("invalid_raid_danger_memory")
            return None
        return plan

    @staticmethod
    def owned_departure(actor):
        role = actor.get("living_role") or {}
        return (actor.get("action") == "MOVE_TO" and actor.get("action_source_goal") == "LOCAL_ACTIVITY" and
            actor.get("goal") == "LOCAL_ACTIVITY" and actor.get("goal_owner") == "INDIVIDUAL" and
            fields.supply.positive_int(actor.get("action_started_at_ms")) and
            actor["action_started_at_ms"] == role.get("started_at_ms") and role.get("phase") == "MOVING" and
            actor.get("movement", {}).get("moving") is True and actor.get("in_combat") is False)

    def observe(self, record):
        prior_frames, prior_epoch = self.field_gate.complete, self.field_gate.epoch
        self.field_gate.observe(record)
        if self.field_gate.epoch != prior_epoch:
            self.events.clear()
            self.stock_sights.clear()
            self.returned.clear()
        if self.field_gate.complete == prior_frames:
            return
        state = record if "agents" in record else record["state"]
        capture = state["captured_at_ms"]
        actors = {actor["spawn_id"]: actor for actor in state["agents"]}
        actor = actors[RAIDER]
        raid, role = actor.get("raid") or {}, actor.get("living_role") or {}
        plan = self.plan(raid)
        if plan is None:
            return
        self.planning_frames += 1
        position = fields.point(actor.get("position"))
        life, guid = role.get("lifetime_ms"), actor.get("runtime_guid")
        source = raid.get("source_depot_id")
        attempt = fields.counter(raid.get("attempt_id"))
        if (source not in SOURCES or attempt is None or position is None or not guid or
            not fields.supply.positive_int(life) or actor.get("alive") is not True or
            actor.get("position", {}).get("source") != "live"):
            return
        binding = (self.field_gate.epoch, guid, life)
        camp = fields.point(self.field_gate.depots[3])
        source_point = fields.point(self.field_gate.depots[source])
        source_stock = (raid.get("source_depot") or {}).get("stock")
        if fields.dist3(position, source_point) <= (1.91 if source in (4, 5, 6) else 3.01) and integer(source_stock, 30):
            self.stock_sights.append(dict(binding=binding, capture=capture, source=source, stock=source_stock))
        if plan["known_stock"] is not None:
            observed_stock_at = capture-plan["stock_age_ms"]
            window = self.field_gate.epochs.get(self.field_gate.epoch)
            if window and observed_stock_at >= window["first_capture"]+OBSERVATION_TIME_TOLERANCE_MS:
                nearby = [sight for sight in self.stock_sights if sight["binding"] == binding and sight["source"] == source and
                          abs(sight["capture"]-observed_stock_at) <= OBSERVATION_TIME_TOLERANCE_MS]
                if not nearby or not any(sight["stock"] == plan["known_stock"] for sight in nearby):
                    self.violation("raid_stock_memory_without_matching_local_observation")
        if (fields.dist3(position, camp) <= 3.01 and (actor.get("supply") or {}).get("cargo") == 0 and
            actor.get("in_combat") is False):
            self.returned[binding] = capture

        # Source-attributed actual victims in current telemetry bind a guard
        # observation. A global guard location or a phase label is insufficient.
        if actor.get("in_combat") is True:
            for guard in actors.values():
                guard_point = fields.point(guard.get("position"))
                if (guard.get("type") == "GUARD" and guard.get("alive") is True and guard.get("in_combat") is True and
                    guard.get("position", {}).get("source") == "live" and guard_point is not None and
                    (guard.get("target") or {}).get("spawn_id") == RAIDER and
                    fields.dist3(position, guard_point) <= NONLOCAL_YARDS):
                    self.events.append(dict(binding=binding, capture=capture, guard=guard["spawn_id"],
                        guard_position=guard_point, source=source, attempt=attempt, route=plan["route_id"]))
        danger = plan.get("danger")
        if danger is None or not plan["memory_records"]:
            return
        observed_at = capture - danger["age_ms"]
        guard_id = fields.counter(danger["guard_spawn_id"])
        point = fields.point(danger["position"])
        same_time = [event for event in self.events if event["binding"] == binding and event["guard"] == guard_id and
                     abs(event["capture"]-observed_at) <= OBSERVATION_TIME_TOLERANCE_MS]
        witnessed = [event for event in same_time if fields.dist3(event["guard_position"], point) <= OBSERVATION_POINT_TOLERANCE_YARDS]
        if same_time and not witnessed:
            self.violation("raid_memory_guard_place_contradicts_observed_encounter")
        if not witnessed:
            return
        event = min(witnessed, key=lambda row: abs(row["capture"]-observed_at))
        returned = self.returned.get(binding, 0)
        if (danger["age_ms"] < MIN_RECALL_AGE_MS or returned <= event["capture"] or
            fields.dist3(position, point) <= NONLOCAL_YARDS):
            return
        deferred = (plan["reason"] in DEFER_REASONS and plan["retry_remaining_ms"] > 0 and
            actor.get("movement", {}).get("moving") is False and actor.get("in_combat") is False and
            fields.dist3(position, camp) <= 3.01 and actor.get("action") in (None, "NONE"))
        changed = source != event["source"] or plan["route_id"] != event["route"]
        departed = attempt > event["attempt"] and changed and raid.get("phase") == "TO_SOURCE" and self.owned_departure(actor)
        if not deferred and not departed:
            return
        key = (binding, attempt, source, plan["route_id"], guard_id, deferred)
        if key in self.closed:
            return
        self.closed.add(key)
        self.decisions.append(dict(epoch=binding[0], runtime_guid=guid, lifetime_ms=life,
            guard_spawn_id=guard_id, observed_combat_capture_ms=event["capture"], remembered_guard_position=danger["position"],
            recalled_age_ms=danger["age_ms"], returned_to_camp_capture_ms=returned,
            decision_capture_ms=capture, previous_source_depot_id=event["source"], source_depot_id=source,
            previous_route_id=event["route"], route_id=plan["route_id"], reason=plan["reason"],
            attempt_id=attempt, decision="DEFERRED" if deferred else "OWNED_DEPARTURE", known_stock=plan["known_stock"]))

    def report(self):
        selected = max(self.field_gate.epochs, key=lambda epoch: (
            sum(receipt["epoch"] == epoch for receipt in self.decisions),
            self.field_gate.epochs[epoch]["last_capture"]-self.field_gate.epochs[epoch]["first_capture"]), default=None)
        window = self.field_gate.epochs.get(selected)
        seconds = (window["last_capture"]-window["first_capture"])/1000 if window else 0
        receipts = [receipt for receipt in self.decisions if receipt["epoch"] == selected]
        missing = []
        if not receipts: missing.append("witnessed_guard_memory_recalled_after_return_for_changed_executed_plan_or_defer")
        if seconds < self.minimum_seconds: missing.append("shared_continuous_fresh_observation_window")
        violations = list(self.field_gate.violations.values())
        status = "FAIL" if violations else "INCONCLUSIVE" if missing else "PASS"
        return dict(profile="fields-v8", criterion="observed_raid_memory_recall_and_later_plan", status=status,
            exit_code=fields.EXIT_CODES[status], expected_owned=40, expected_native=60,
            samples=self.field_gate.samples, fresh_samples=self.field_gate.fresh, planning_frames=self.planning_frames,
            selected_epoch=selected, observed_seconds=seconds, minimum_observation_seconds=self.minimum_seconds,
            minimum_recalled_age_ms=MIN_RECALL_AGE_MS, decisions=self.decisions,
            quality=dict(self.field_gate.quality), violations=violations, missing_evidence=missing,
            limitations=[
                "PASS proves a witnessed guard encounter, later nonlocal recall after camp return and changed owned departure or actual defer. It does not independently prove the planner's counterfactual ranking.",
                "A remembered guard location is bound to its observed place, independently of the currently selected warehouse. Successful theft, arrival at a warehouse, group combat and LOS rays are not certified.",
                "All forty identities and six-depot food flows use the unchanged field ledger checks; complete farmer meal-return cycles are not required here.",
                "Snapshot timing bounds recollection age. Evidence from before the recorded epoch, another incarnation or a missing observed attacker cannot qualify."])


def evaluate_samples(samples: Iterable[dict], *, minimum_seconds=300, max_gap_seconds=2):
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds)
    for sample in samples:
        evaluator.observe(sample)
    return evaluator.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=300)
    parser.add_argument("--max-gap-seconds", type=float, default=2)
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(fields.recording_rows(args.path), minimum_seconds=args.minimum_seconds, max_gap_seconds=args.max_gap_seconds)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile="fields-v8", status="INCONCLUSIVE", exit_code=2,
                      missing_evidence=["readable_valid_recording"], input_error=type(error).__name__)
    rendered = json.dumps(report,ensure_ascii=False,allow_nan=False,indent=2)+"\n"
    if args.output: args.output.write_text(rendered,encoding="utf-8")
    print(rendered,end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
