"""Read-only physical Defias theft proof for the forty-owner fields-v8 world.

All six depots and every food participant are reconciled by the existing field
ledger gate. Each requested source needs a linked owned theft, visible carry,
and owned delivery into the Defias camp in one uninterrupted observed epoch.
This gate does not replace the legacy hunter, consumer, or guard-defence gates.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable

try:
    from . import field_acceptance as fields, raid_acceptance as raids
except ImportError:
    import field_acceptance as fields
    import raid_acceptance as raids

VILLAGE_WAREHOUSE_IDS = frozenset((1, 4, 5, 6))


class _Evaluator(raids._Evaluator):
    def __init__(self, minimum_seconds=300, max_gap_seconds=2, *, required_sources=None):
        super().__init__(minimum_seconds, max_gap_seconds)
        requested = tuple(sorted(VILLAGE_WAREHOUSE_IDS)) if required_sources is None else tuple(required_sources)
        if (not requested or any(type(identity) is not int or identity not in VILLAGE_WAREHOUSE_IDS for identity in requested)
            or len(set(requested)) != len(requested)):
            raise ValueError("required sources must be unique village warehouse IDs 1, 4, 5, 6")
        self.required_sources = frozenset(requested)
        self.raid_source_depot_ids = VILLAGE_WAREHOUSE_IDS
        self.field_gate = fields._Evaluator(minimum_seconds, max_gap_seconds)
        self.depots = self.field_gate.depots
        # Both physical gates contribute to the same result. Never hide farmer
        # or shared-ledger failures when reporting a successful theft receipt.
        self.quality = self.field_gate.quality
        self.violations = self.field_gate.violations

    def work_radius(self, row, phase):
        # Field warehouses share the runtime's tighter body-clear anchor.
        return 1.91 if phase == "STEALING" and row["raid"]["source_depot_id"] in (4, 5, 6) else 3.01

    def observe(self, record):
        previous_complete, previous_epoch = self.field_gate.complete, self.field_gate.epoch
        self.field_gate.observe(record)
        self.epoch = self.field_gate.epoch
        if self.epoch != previous_epoch:
            self.update_raid({})
        if self.field_gate.complete == previous_complete:
            return
        state = record if "agents" in record else record["state"]
        capture = state["captured_at_ms"]
        t = capture / 1000 if "agents" in record else record["elapsed_seconds"]
        actors = {actor["spawn_id"]: actor for actor in state["agents"]}
        rows = {sid: self.row(actors[sid], t, capture) for sid in (raids.RAIDER, raids.GUARD)}
        rows = {sid: row for sid, row in rows.items() if row is not None}
        for row in rows.values():
            row["depot_flow"] = self.field_gate.last_flows[row["depot"]["id"]]
            row["source_depot_flow"] = self.field_gate.last_flows[row["raid"]["source_depot_id"]]
        self.update_raid(rows)

    def report(self):
        successes = self.raid_track.successes
        selected = max(self.field_gate.epochs, key=lambda epoch: (
            len({receipt["source_depot_id"] for receipt in successes if receipt["epoch"] == epoch} & self.required_sources),
            self.field_gate.epochs[epoch]["last_capture"] - self.field_gate.epochs[epoch]["first_capture"]), default=None)
        cycles = [receipt for receipt in successes if receipt["epoch"] == selected]
        covered = {receipt["source_depot_id"] for receipt in cycles}
        window = self.field_gate.epochs.get(selected)
        seconds = (window["last_capture"] - window["first_capture"]) / 1000 if window else 0
        missing = [f"owned_theft_carry_camp_delivery_from_warehouse_{identity}" for identity in sorted(self.required_sources-covered)]
        if seconds < self.minimum_seconds:
            missing.append("shared_continuous_fresh_observation_window")
        violations = list(self.violations.values())
        status = "FAIL" if violations else "INCONCLUSIVE" if missing else "PASS"
        return dict(profile="fields-v8", criterion="physical_defias_theft_from_requested_village_warehouses",
            status=status, exit_code=fields.EXIT_CODES[status], expected_owned=40, expected_native=60,
            samples=self.field_gate.samples, fresh_samples=self.field_gate.fresh,
            complete_population_frames=self.field_gate.complete, selected_epoch=selected,
            observed_seconds=seconds, minimum_observation_seconds=self.minimum_seconds,
            max_gap_seconds=self.max_gap, required_source_depot_ids=sorted(self.required_sources),
            eligible_source_depot_ids=sorted(VILLAGE_WAREHOUSE_IDS), completed_source_depot_ids=sorted(covered),
            unobserved_source_depot_ids=sorted(VILLAGE_WAREHOUSE_IDS-covered),
            raid_cycles=successes, prevented_raids=self.raid_track.prevented,
            pending_raid=self.raid_track.attempt, quality=dict(self.quality), violations=violations,
            missing_evidence=missing, limitations=[
                "PASS covers only the explicitly required source warehouse IDs; unobserved sources are listed separately.",
                "Each theft/delivery requires continuously observed owned five-second work, source debit, carry and camp credit with one unchanged source per attempt.",
                "All six depots and forty managed identities are checked using the field ledger gate. Completed farmer meal-return cycles are not a prerequisite of this theft gate.",
                "This gate does not certify the separate hunter, consumer or guard-defence gates, client animation pixels, collision rays or line of sight.",
                "Bounded snapshots establish observed action continuity; receipt capture age bounds the debit time rather than independently timestamping it."])


def evaluate_samples(samples: Iterable[dict], *, minimum_seconds=300, max_gap_seconds=2, required_sources=None):
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds, required_sources=required_sources)
    for sample in samples:
        evaluator.observe(sample)
    return evaluator.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="closed recorder directory, .jsonl or .jsonl.gz")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=300)
    parser.add_argument("--max-gap-seconds", type=float, default=2)
    parser.add_argument("--required-source", type=int, action="append", dest="required_sources",
                        help="require this warehouse; repeat for several (default: all four)")
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(fields.recording_rows(args.path), minimum_seconds=args.minimum_seconds,
                                  max_gap_seconds=args.max_gap_seconds, required_sources=args.required_sources)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile="fields-v8", status="INCONCLUSIVE", exit_code=2,
                      missing_evidence=["readable_valid_recording"], input_error=type(error).__name__)
    rendered = json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2)+"\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
