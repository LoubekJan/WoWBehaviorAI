"""Read-only physical acceptance of wildlife-v8 Observer JSONL recordings.

evaluate_samples accepts recorder sample rows or /api/state dictionaries. Old
recordings without runtime_guid and living_role.lifetime_ms remain usable for
diagnostics, but cannot prove a continuous physical life. A matched live
alive-to-dead target transition proves an observed hunted-target death, not the
identity of the killing blow or absence of manual intervention.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import gzip
import json
import math
from pathlib import Path
from typing import Iterable

OWNED_IDS = frozenset(range(901072, 901092))
PREDATOR_IDS = frozenset((901076, 901081, 901086, 901091))
PREY_IDS = OWNED_IDS - PREDATOR_IDS
EXIT_CODES = {"PASS": 0, "INCONCLUSIVE": 2, "FAIL": 3}
PHASES = ("HUNTING", "TARGET_DEATH", "FEEDING", "FED", "RETURN_HOME", "ARRIVED")
MANIFEST = Path(__file__).resolve().parents[2] / "data/realm_lab/aiworldlab/preview-v8/settlement-population.json"


def finite(value) -> bool:
    return type(value) in (int, float) and math.isfinite(value)


def positive_int(value) -> bool:
    return type(value) is int and value > 0


def point(value) -> tuple[float, float, float] | None:
    if isinstance(value, dict) and all(finite(value.get(axis)) for axis in ("x", "y", "z")):
        return tuple(value[axis] for axis in ("x", "y", "z"))
    return None


def distance(a, b) -> float:
    return math.hypot(a[0] - b[0], a[1] - b[1])


@dataclass
class Track:
    role: str
    samples: int = 0
    live_samples: int = 0
    previous: dict | None = None
    anchor: tuple | None = None
    started: float | None = None
    max_continuous_seconds: float = 0
    max_displacement: float = 0
    movement: dict | None = None
    grazing: dict | None = None
    forage: dict | None = None
    forage_anchor: tuple | None = None
    phases: set = field(default_factory=set)
    interruptions: Counter = field(default_factory=Counter)
    cycle: dict | None = None
    best_partial: dict | None = None
    cycles: list = field(default_factory=list)


class _Evaluator:
    def __init__(self, minimum_seconds: float, max_gap_seconds: float):
        if not finite(minimum_seconds) or minimum_seconds <= 0 or not finite(max_gap_seconds) or max_gap_seconds <= 0:
            raise ValueError("observation windows must be finite and positive")
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.homes = {a["spawn_id"]: a["home"] for a in manifest["actors"] if a["spawn_id"] in OWNED_IDS}
        self.bounds = manifest["bounds"]
        self.minimum_seconds, self.max_gap = minimum_seconds, max_gap_seconds
        self.tracks = {sid: Track("PREDATOR" if sid in PREDATOR_IDS else "PREY") for sid in sorted(OWNED_IDS)}
        self.quality = Counter()
        self.violations = {}
        self.samples = self.fresh = self.population_frames = 0
        self.session = self.previous_session = self.last_capture = self.last_time = self.last_sequence = None
        self.epoch = 0
        self.direct_sequence = 0
        self.last_rows = {}

    def violation(self, code: str, sid: int):
        self.violations[(code, sid)] = {"code": code, "spawn_id": sid}

    def interrupt(self, sid: int, reason: str):
        track = self.tracks[sid]
        if track.previous is not None or track.cycle is not None:
            track.interruptions[reason] += 1
        self.archive_cycle(track, reason)
        track.previous = track.anchor = track.started = track.forage_anchor = None

    def reset(self, reason: str):
        self.epoch += 1
        self.quality[reason] += 1
        self.last_rows = {}
        for sid in self.tracks:
            self.interrupt(sid, reason)

    @staticmethod
    def archive_cycle(track: Track, reason: str):
        if track.cycle:
            partial = {**track.cycle, "interrupted_by": reason}
            if track.best_partial is None or len(partial["covered"]) > len(track.best_partial["covered"]):
                track.best_partial = partial
            track.cycle = None

    def live_row(self, agent: dict, sid: int, t: float, capture: int) -> dict | None:
        track = self.tracks[sid]
        track.samples += 1
        if agent.get("agent_id") != sid:
            self.violation("agent_spawn_identity_mismatch", sid)
            return None
        if agent.get("native_creature") is not False or agent.get("control_mode") != "AI_WORLD_CONTROLLED":
            self.violation("wildlife_not_controlled", sid)
            return None
        if agent.get("entry") != (1186 if sid in PREDATOR_IDS else 883) or agent.get("type") != track.role:
            self.violation("wrong_wildlife_role_or_entry", sid)
            return None
        home = agent.get("home")
        expected = self.homes[sid]
        if point(home) is None:
            self.quality["missing_home"] += 1
            return None
        if home.get("map_id") != 725 or any(abs(home[axis] - expected[axis]) > 0.01 for axis in ("x", "y", "z")):
            self.violation("home_changed", sid)
            return None
        pos = agent.get("position")
        if not isinstance(pos, dict) or pos.get("source") != "live":
            self.quality["spawn_or_last_known_fallback"] += 1
            return None
        xyz = point(pos)
        if xyz is None or type(agent.get("alive")) is not bool or not finite(agent.get("health")):
            self.quality["invalid_live_observation"] += 1
            return None
        if pos.get("map_id") != 725 or not (self.bounds["min_x"] <= xyz[0] <= self.bounds["max_x"] and
                                            self.bounds["min_y"] <= xyz[1] <= self.bounds["max_y"]):
            self.violation("outside_map_or_bounds", sid)
            return None
        alive = agent["alive"]
        if (alive and agent["health"] <= 0) or (not alive and agent["health"] != 0):
            self.quality["inconsistent_health_and_alive"] += 1
            return None
        role = agent.get("living_role") or {}
        needs = agent.get("needs") or {}
        movement = agent.get("movement") or {}
        if not all(isinstance(v, dict) for v in (role, needs, movement)):
            self.quality["invalid_live_observation"] += 1
            return None
        if alive and (role.get("enabled") is not True or role.get("role") != track.role):
            self.quality["missing_live_role"] += 1
            return None
        guid = agent.get("runtime_guid")
        if not isinstance(guid, str) or not guid.strip():
            self.quality["missing_runtime_guid"] += 1
            return None
        life = role.get("lifetime_ms")
        # Death resets LivingRole, so a dead prey's GUID alone identifies the
        # same corpse; lifetime is mandatory for live motion and predators.
        if alive and (not positive_int(life) or life > capture):
            self.quality["missing_lifetime_ms"] += 1
            return None
        hunger = needs.get("hunger")
        home_distance = movement.get("home_distance")
        if alive and (not finite(hunger) or not 0 <= hunger <= 1 or not finite(home_distance) or home_distance < 0):
            self.quality["missing_hunger_or_home_distance"] += 1
            return None
        # Runtime home is an independent engine anchor. A persisted home at
        # the correct coordinates must not hide a relocated runtime home.
        if alive and abs(home_distance - distance(xyz, point(home))) > 1:
            self.violation("runtime_home_distance_inconsistent", sid)
            return None
        if agent.get("world_state") != "MATERIALIZED":
            self.quality["not_materialized"] += 1
            return None
        track.live_samples += 1
        return {"agent": agent, "role": role, "position": xyz, "guid": guid, "life": life,
                "alive": alive, "hunger": hunger, "home_distance": home_distance, "t": t,
                "capture": capture, "epoch": self.epoch}

    def observe(self, record: dict):
        if not isinstance(record, dict):
            self.reset("invalid_record")
            return
        if record.get("kind") == "session":
            self.session = record.get("session_id")
            return
        if record.get("kind") == "summary":
            return
        if "state" not in record and "agents" in record:
            self.direct_sequence += 1
            capture = record.get("captured_at_ms")
            record = {"state": record, "status": "fresh", "sequence": self.direct_sequence,
                      "elapsed_seconds": capture / 1000 if finite(capture) else None}
        self.samples += 1
        state = record.get("state")
        t, sequence = record.get("elapsed_seconds"), record.get("sequence")
        if (not isinstance(state, dict) or not finite(t) or t < 0 or not positive_int(sequence)):
            self.reset("invalid_sample")
            return
        session = record.get("session_id", self.session)
        if self.last_time is not None and (session != self.previous_session or sequence != self.last_sequence + 1 or
                                          not 0 < t - self.last_time <= self.max_gap):
            self.reset("session_sequence_or_time_gap")
        self.previous_session, self.last_time, self.last_sequence = session, t, sequence
        capture = state.get("captured_at_ms")
        if (record.get("status") != "fresh" or state.get("configured") is not True or state.get("stale") is not False or
            state.get("version") not in (2, 3, 4) or
            not positive_int(capture) or type(state.get("age_ms")) is not int or not 0 <= state["age_ms"] < 5000 or
            not isinstance(state.get("agents"), list)):
            self.reset("stale_or_unusable_snapshot")
            return
        if self.last_capture is not None and not 0 < capture - self.last_capture <= self.max_gap * 1000:
            self.reset("capture_repeat_reset_or_gap")
        self.last_capture = capture
        agents = state["agents"]
        ids = [a.get("spawn_id") for a in agents if isinstance(a, dict)]
        if len(ids) != len(agents) or any(not positive_int(i) for i in ids) or len(set(ids)) != len(ids):
            self.reset("invalid_or_duplicate_spawn_id")
            return
        agent_ids = [a.get("agent_id") for a in agents]
        if any(not positive_int(i) for i in agent_ids) or len(set(agent_ids)) != len(agent_ids):
            self.reset("invalid_or_duplicate_agent_id")
            return
        self.fresh += 1
        by_id = {a["spawn_id"]: a for a in agents}
        for sid, agent in by_id.items():
            if sid not in OWNED_IDS and agent.get("control_mode") == "AI_WORLD_CONTROLLED":
                self.violation("unexpected_controlled_spawn", sid)
        population_valid = True
        current = {}
        for sid in self.tracks:
            if sid not in by_id:
                self.quality["missing_owned_spawn"] += 1
                self.interrupt(sid, "missing_owned_spawn")
                population_valid = False
                continue
            row = self.live_row(by_id[sid], sid, t, capture)
            if row is None:
                self.interrupt(sid, "unusable_agent_observation")
                population_valid = False
                continue
            current[sid] = row
        if population_valid:
            self.population_frames += 1
        for sid, row in current.items():
            self.update_track(sid, row)
        for sid in PREDATOR_IDS:
            if sid in current:
                self.update_cycle(sid, current[sid], current)
        self.last_rows = current

    def update_track(self, sid: int, row: dict):
        track = self.tracks[sid]
        old = track.previous
        if old and ((old["guid"], old["life"]) != (row["guid"], row["life"]) or not old["alive"] or not row["alive"]):
            self.interrupt(sid, "incarnation_or_death")
            old = None
        if not row["alive"]:
            return
        if old is None:
            track.anchor, track.started = row["position"], row["t"]
        displacement = distance(track.anchor, row["position"])
        track.max_displacement = max(track.max_displacement, displacement)
        track.max_continuous_seconds = max(track.max_continuous_seconds, row["t"] - track.started)
        if displacement >= 2 and track.movement is None:
            track.movement = {"epoch": row["epoch"], "runtime_guid": row["guid"], "lifetime_ms": row["life"],
                              "start_seconds": track.started, "end_seconds": row["t"], "yards": round(displacement, 3)}
        role = row["role"]
        track.phases.add(str(role.get("phase", "UNKNOWN")))
        if (sid in PREY_IDS and old and old["role"].get("phase") == "ACTING" and
            old["role"].get("activity") == "GRAZE" and old["hunger"] - row["hunger"] >= 0.1):
            track.grazing = {"epoch": row["epoch"], "runtime_guid": row["guid"], "lifetime_ms": row["life"],
                             "start_seconds": old["t"], "end_seconds": row["t"],
                             "hunger_before": old["hunger"], "hunger_after": row["hunger"]}
        if role.get("movement_purpose") == "FORAGE_SEARCH":
            if track.forage_anchor is None:
                track.forage_anchor = row["position"], row["t"]
            travel = distance(track.forage_anchor[0], row["position"])
            if track.forage is None or travel >= track.forage["travel_yards"]:
                track.forage = {"observed": True, "travel_yards": round(travel, 3), "movement_confirmed": travel >= 2,
                                "epoch": row["epoch"], "runtime_guid": row["guid"], "lifetime_ms": row["life"],
                                "start_seconds": track.forage_anchor[1], "end_seconds": row["t"]}
        else:
            track.forage_anchor = None
        track.previous = row

    def update_cycle(self, sid: int, row: dict, current: dict):
        track = self.tracks[sid]
        if not row["alive"]:
            self.archive_cycle(track, "predator_dead")
            return
        role, agent = row["role"], row["agent"]
        target = agent.get("target") or {}
        target_id = target.get("spawn_id") if isinstance(target, dict) else None
        if role.get("phase") == "HUNTING":
            attempt = role.get("started_at_ms")
            if target_id not in PREY_IDS or not positive_int(attempt) or attempt > row["capture"]:
                self.archive_cycle(track, "unresolved_hunt_target_or_attempt")
                return
            if track.cycle and (track.cycle["prey_spawn_id"] != target_id or track.cycle["hunt_started_at_ms"] != attempt):
                self.archive_cycle(track, "new_hunt_attempt")
            if track.cycle is None:
                prey = current.get(target_id)
                if prey is None or not prey["alive"]:
                    track.interruptions["hunt_without_live_prey"] += 1
                    return
                track.cycle = {"predator_spawn_id": sid, "prey_spawn_id": target_id, "prey_runtime_guid": prey["guid"],
                               "prey_lifetime_ms": prey["life"],
                               "runtime_guid": row["guid"], "lifetime_ms": row["life"], "epoch": row["epoch"],
                               "hunt_started_at_ms": attempt, "covered": ["HUNTING"], "times": {"HUNTING": row["t"]}}
        cycle = track.cycle
        if cycle is None:
            return
        if (cycle["runtime_guid"], cycle["lifetime_ms"], cycle["epoch"]) != (row["guid"], row["life"], row["epoch"]):
            self.archive_cycle(track, "predator_incarnation_changed")
            return
        prey_id = cycle["prey_spawn_id"]
        prey, old_prey = current.get(prey_id), self.last_rows.get(prey_id)
        if "FED" not in cycle["covered"]:
            if prey is None or prey["guid"] != cycle["prey_runtime_guid"]:
                self.archive_cycle(track, "prey_missing_or_incarnation_changed")
                return
            if prey["alive"] and ("TARGET_DEATH" in cycle["covered"] or prey["life"] != cycle["prey_lifetime_ms"]):
                self.archive_cycle(track, "prey_respawn_or_lifetime_changed")
                return
            if ("TARGET_DEATH" not in cycle["covered"] and old_prey and old_prey["alive"] and not prey["alive"] and
                old_prey["guid"] == prey["guid"] and role.get("phase") in ("HUNTING", "FEEDING") and target_id == prey_id):
                cycle["covered"].append("TARGET_DEATH")
                cycle["times"]["TARGET_DEATH"] = row["t"]
            if (role.get("phase") == "FEEDING" and agent.get("action") == "EAT" and
                agent.get("action_source_goal") == "FEED" and target_id == prey_id and not prey["alive"] and
                "TARGET_DEATH" in cycle["covered"]):
                if "FEEDING" not in cycle["covered"]:
                    cycle["covered"].append("FEEDING")
                    cycle["times"]["FEEDING"] = row["t"]
                    cycle["feeding_started_at_ms"] = role.get("started_at_ms")
                    cycle["hunger_before"] = row["hunger"]
            if ("FEEDING" in cycle["covered"] and role.get("hunt_end") == "FED" and
                cycle["hunger_before"] - row["hunger"] >= 0.1 and
                positive_int(cycle["feeding_started_at_ms"]) and row["capture"] - cycle["feeding_started_at_ms"] >= 5000):
                cycle["covered"].append("FED")
                cycle["times"]["FED"] = row["t"]
                cycle["hunger_after"] = row["hunger"]
        if "FED" in cycle["covered"] and role.get("movement_purpose") == "RETURN_HOME" and row["home_distance"] > 14:
            if "RETURN_HOME" not in cycle["covered"]:
                cycle["covered"].append("RETURN_HOME")
                cycle["times"]["RETURN_HOME"] = row["t"]
                cycle["return_start_position"] = row["position"]
                cycle["return_start_distance"] = row["home_distance"]
        if ("RETURN_HOME" in cycle["covered"] and row["home_distance"] <= 14 and
            cycle["return_start_distance"] - row["home_distance"] >= 2 and
            distance(cycle["return_start_position"], row["position"]) >= 2):
            cycle["covered"].append("ARRIVED")
            cycle["times"]["ARRIVED"] = row["t"]
            cycle["return_end_distance"] = row["home_distance"]
            cycle["death_evidence"] = "observed_live_target_alive_to_dead_same_runtime_guid"
            if len(track.cycles) < 32:
                track.cycles.append(cycle)
            track.cycle = None

    def report(self) -> dict:
        agents, cycles, missing = {}, [], []
        for sid, track in self.tracks.items():
            partial = track.cycle or track.best_partial
            phases = list(PHASES) if track.cycles else partial["covered"] if partial else []
            needed = []
            if track.movement is None:
                needed.append("continuous_live_movement_2_yards")
                if track.max_continuous_seconds >= self.minimum_seconds:
                    self.violation("no_observed_movement_during_full_window", sid)
            if track.live_samples == 0:
                needed.append("live_identity_observations")
            agents[str(sid)] = {"role": track.role, "samples": track.samples, "live_samples": track.live_samples,
                                "movement": track.movement, "maximum_displacement_yards": round(track.max_displacement, 3),
                                "continuous_seconds": round(track.max_continuous_seconds, 3),
                                "observed_phases": sorted(track.phases), "grazing_hunger_drop": track.grazing,
                                "forage": track.forage, "cycle_count": len(track.cycles),
                                "partial_cycle": partial, "covered_cycle_phases": phases,
                                "missing_cycle_phases": [p for p in PHASES if p not in phases] if sid in PREDATOR_IDS and not track.cycles else [],
                                "missing": needed, "interruptions": dict(track.interruptions)}
            cycles.extend(track.cycles)
        if not self.population_frames:
            missing.append("one_fresh_frame_with_all_20_controlled_live_identities_and_roles")
        if any(t.movement is None for t in self.tracks.values()):
            missing.append("physical_movement_at_least_2_yards_for_all_20")
        if not any(self.tracks[sid].grazing for sid in PREY_IDS):
            missing.append("observed_prey_grazing_then_hunger_drop")
        if not cycles:
            missing.append("one_continuous_observed_hunt_death_feed_hunger_drop_return_cycle")
        violations = list(self.violations.values())
        status = "FAIL" if violations else "INCONCLUSIVE" if missing else "PASS"
        return {"profile": "wildlife-v8", "status": status, "exit_code": EXIT_CODES[status],
                "samples": self.samples, "fresh_samples": self.fresh, "complete_population_frames": self.population_frames,
                "expected_owned": 20, "expected_predators": 4, "expected_prey": 16,
                "cycles": cycles, "agents": agents, "missing_evidence": missing, "violations": violations,
                "quality": dict(self.quality), "minimum_stationary_observation_seconds": self.minimum_seconds,
                "forage_901086": self.tracks[901086].forage,
                "limitations": ["Target death is observed, but killing-blow attribution and absence of manual intervention are not exported.",
                                "A sampled FED string, counters, spawn fallback or a hunger drop alone never proves a cycle."]}


def evaluate_samples(samples: Iterable[dict], *, minimum_seconds: float = 900, max_gap_seconds: float = 5) -> dict:
    """Evaluate ordered recorder sample rows or exported /api/state values.

    PASS requires one observed complete cycle, movement of every owned actor,
    and prey grazing with a hunger drop. Missing identity/fresh observations
    produce INCONCLUSIVE; explicit ownership/home/scope violations or a full
    continuous stationary observation window produce FAIL.
    """
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds)
    for sample in samples:
        evaluator.observe(sample)
    return evaluator.report()


def recording_rows(path: Path) -> Iterable[dict]:
    """Read JSONL/plain gzip or ordered recorder parts without changing them."""
    if path.is_dir():
        summary_path = path / "summary.json"
        if summary_path.is_file():
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            names = summary.get("parts", [])
            if not names or any(not isinstance(n, str) or Path(n).name != n or "/" in n or "\\" in n for n in names):
                raise ValueError("invalid recording parts")
            parts = [path / name for name in names]
        else:
            parts = sorted(path.glob("part-*.jsonl*"))
        if not parts:
            raise ValueError("recording has no parts")
    else:
        parts = [path]
    for part in parts:
        opener = gzip.open if part.suffix == ".gz" else open
        with opener(part, "rt", encoding="utf-8") as stream:
            while line := stream.readline(64 * 1024 * 1024 + 1):
                if len(line) > 64 * 1024 * 1024:
                    raise ValueError("recording line exceeds 64 MiB")
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise ValueError("recording row must be an object")
                yield row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="recorder directory, .jsonl or .jsonl.gz")
    parser.add_argument("--output", type=Path, help="optional JSON report file; stdout always receives JSON")
    parser.add_argument("--minimum-seconds", type=float, default=900)
    parser.add_argument("--max-gap-seconds", type=float, default=5)
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(recording_rows(args.path), minimum_seconds=args.minimum_seconds,
                                  max_gap_seconds=args.max_gap_seconds)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = {"profile": "wildlife-v8", "status": "INCONCLUSIVE", "exit_code": 2,
                  "missing_evidence": ["readable_valid_recording"], "input_error": type(error).__name__}
    rendered = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
