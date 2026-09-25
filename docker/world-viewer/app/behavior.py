"""Read-only acceptance checks for Observer recordings; no gameplay commands."""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import gzip
import json
import math
from pathlib import Path
import sys


@dataclass(frozen=True)
class Policy:
    minimum_seconds: float = 3600
    fresh_fraction: float = 0.95
    return_seconds: float = 300
    motion_seconds: float = 60
    outside_seconds: float = 30
    stock_hunger_seconds: float = 600
    empty_stock_seconds: float = 600
    predator_hunger_seconds: float = 1800
    position_tolerance: float = 1


CHECKS = {
    "return": "Návrat bez dlouhého zablokování",
    "motion": "Pohybový úkol se skutečným posunem",
    "outside": "Zachování řízení v Elwynnu",
    "stock_hunger": "Jídlo při hladu a dostupných zásobách",
    "empty_stock": "Obnovení prázdné zásoby jídla u pracovní rutiny",
    "predator_hunger": "Dlouhodobý hlad predátorů (upozornění)",
}
RADII = {"PREDATOR": 12, "PREY": 6, "GUARD": 8, "COMBATANT": 10,
         "CIVILIAN": 4, "WORKER": 4, "TRAVELER": 12, "SERVICE": 0}
SCOPED = {"READY", "ACTIVE", "CURATED_ROUTINE", "GROUP_ACTIVITY"}
LOCAL_MOVES = {"RETURN_HOME", "FORAGE_SEARCH", "LOCAL_ROAM", "HERD_COHESION", "PATROL_COMPANION", "FOOD_SUPPLY"}


def finite(value) -> bool:
    return type(value) in (int, float) and math.isfinite(value)


class Evaluator:
    """Keep only adjacent observations, open episodes and each agent's worst case."""

    def __init__(self, metadata: dict, policy: Policy | None = None):
        self.metadata = metadata
        self.policy = policy or Policy()
        interval = metadata.get("interval_seconds", 5)
        if not finite(interval) or interval <= 0:
            raise ValueError("invalid recording interval")
        self.max_gap = max(1.0, interval * 1.5)
        self.counts = Counter()
        self.quality = Counter()
        self.previous = {}
        self.runs = {}
        self.worst = {}
        self.observed = {key: set() for key in CHECKS}
        self.roles = {}
        self.inside = set()
        self.last_time = self.last_capture = self.last_sequence = None
        self.first_time = None
        self.fresh_seconds = 0.0
        self.live_samples = 0
        self.samples = 0
        self.last_was_fresh = False

    def break_continuity(self):
        self.previous.clear()
        self.runs.clear()
        self.inside.clear()
        self.last_was_fresh = False

    def episode(self, key, agent, row, condition, *, stationary=False, start=True):
        identity = (agent["agent_id"], key)
        if not condition:
            self.runs.pop(identity, None)
            return
        point = tuple(agent["position"][axis] for axis in ("x", "y", "z"))
        run = self.runs.get(identity)
        if run and stationary and math.dist(point, run["position"]) > self.policy.position_tolerance:
            self.runs.pop(identity)
            run = None
        if run is None:
            if not start:
                return
            run = {"agent_id": agent["agent_id"], "spawn_id": agent["spawn_id"], "name": agent.get("name"),
                   "role": agent["living_role"]["role"], "start_seconds": row["elapsed_seconds"],
                   "start_utc": row.get("recorded_at_utc"), "position": point}
            self.runs[identity] = run
        duration = row["elapsed_seconds"] - run["start_seconds"]
        limit = getattr(self.policy, key + "_seconds" if key != "return" else "return_seconds")
        if duration < limit:
            return
        detail = {**run, "check": key, "duration_seconds": round(duration, 3),
                  "end_utc": row.get("recorded_at_utc"), "end_seconds": row["elapsed_seconds"],
                  "movement_purpose": agent["living_role"]["movement_purpose"],
                  "home_distance": agent["movement"]["home_distance"],
                  "severity": "warning" if key == "predator_hunger" else "failure"}
        recovery = agent["living_role"].get("return_recovery")
        if isinstance(recovery, dict):
            detail["return_recovery"] = recovery
        if duration > self.worst.get(identity, {}).get("duration_seconds", -1):
            self.worst[identity] = detail

    def observe(self, row: dict):
        self.samples += 1
        status = row.get("status", "invalid")
        self.counts[status] += 1
        t, seq = row.get("elapsed_seconds"), row.get("sequence")
        if not finite(t) or t < 0 or type(seq) is not int or seq < 1:
            self.quality["invalid_sample"] += 1
            self.break_continuity()
            return
        continuous = self.last_was_fresh
        if seq != (self.last_sequence or 0) + 1:
            self.quality["sequence_gap"] += 1
            continuous = False
        dt = 0 if self.last_time is None else t - self.last_time
        if self.last_time is not None and not 0 < dt <= self.max_gap:
            self.quality["time_gap_or_reset"] += 1
            continuous = False
        if self.first_time is None:
            self.first_time = t
        self.last_time, self.last_sequence = t, seq
        state = row.get("state") or {}
        if not isinstance(state, dict):
            self.quality["invalid_snapshot"] += 1
            self.break_continuity()
            return
        capture = state.get("captured_at_ms")
        valid = (status == "fresh" and state.get("configured") is True and state.get("stale") is False
                 and state.get("version") in (2, 3, 4) and type(capture) is int and capture >= 0
                 and type(state.get("age_ms")) is int and 0 <= state["age_ms"] <= 5000
                 and isinstance(state.get("agents"), list) and 0 < len(state["agents"]) <= 10000)
        if not valid:
            if status == "fresh":
                self.quality["invalid_fresh_snapshot"] += 1
            self.break_continuity()
            return
        if self.last_capture is not None and capture <= self.last_capture:
            self.quality["capture_repeat_or_reset"] += 1
            self.last_capture = capture
            self.break_continuity()
            return
        self.last_capture = capture
        if not continuous:
            self.break_continuity()
        else:
            self.fresh_seconds += dt
        ids = [a.get("agent_id") for a in state["agents"] if isinstance(a, dict)]
        if (len(ids) != len(state["agents"]) or any(type(i) is not int or i < 0 for i in ids)
                or len(ids) != len(set(ids))):
            self.quality["invalid_or_duplicate_agent_id"] += 1
            self.break_continuity()
            return
        current = {}
        for agent in state["agents"]:
            aid = agent["agent_id"]
            pos, role, move = (agent.get(k) or {} for k in ("position", "living_role", "movement"))
            if not all(isinstance(value, dict) for value in (pos, role, move)):
                self.quality["invalid_live_agent"] += 1
                continue
            if (agent.get("alive") is not True or pos.get("source") != "live"
                    or agent.get("control_mode") != "AI_WORLD_CONTROLLED" or role.get("enabled") is not True
                    or agent.get("living_wolf") is True or role.get("status") == "WOLF_PACK_CYCLE"):
                continue
            needs, economy = agent.get("needs") or {}, agent.get("economy") or {}
            if not isinstance(needs, dict) or not isinstance(economy, dict):
                self.quality["invalid_live_agent"] += 1
                continue
            if (role.get("role") not in RADII or role.get("status") not in SCOPED | {"OUTSIDE_ELWYNN"}):
                continue
            if (any(not finite(pos.get(k)) for k in ("x", "y", "z")) or type(pos.get("map_id")) is not int
                    or type(agent.get("spawn_id")) is not int or type(agent.get("in_combat")) is not bool
                    or any(type(move.get(k)) is not bool for k in ("moving", "blocked", "evading"))
                    or not finite(move.get("home_distance")) or move["home_distance"] < 0
                    or not finite(needs.get("hunger")) or not 0 <= needs["hunger"] <= 1
                    or any(type(economy.get(k)) is not int or economy[k] < 0 for k in ("food", "resource"))
                    or any(not isinstance(role.get(k), str) for k in ("phase", "activity", "movement_purpose"))):
                self.quality["invalid_live_agent"] += 1
                continue
            old = self.previous.get(aid)
            if old and (old["spawn_id"] != agent["spawn_id"] or old["position"]["map_id"] != pos["map_id"]
                        or old["living_role"]["role"] != role["role"]):
                for key in CHECKS:
                    self.runs.pop((aid, key), None)
                self.inside.discard(aid)
                old = None
            current[aid] = agent
            self.live_samples += 1
            metrics = self.roles.setdefault(role["role"], Counter())
            metrics["live_samples"] += 1
            metrics["hungry_samples"] += needs["hunger"] >= 0.95
            metrics["moving_samples"] += move["moving"]
            if old:
                old_role = old["living_role"]
                metrics["hunger_drops"] += old["needs"]["hunger"] - needs["hunger"] > 0.1
                for field in ("food", "resource"):
                    delta = economy[field] - old["economy"][field]
                    metrics[field + ("_increase" if delta > 0 else "_decrease")] += abs(delta)
                for phase in ("HUNTING", "FEEDING", "DEFENDING"):
                    metrics[phase.lower() + "_starts"] += role["phase"] == phase and old_role["phase"] != phase
            in_scope = role["status"] in SCOPED and pos["map_id"] == 0
            if in_scope:
                self.inside.add(aid)
                self.observed["outside"].add(aid)
            outside = aid in self.inside and role["status"] == "OUTSIDE_ELWYNN"
            self.episode("outside", agent, row, outside)
            calm = in_scope and not agent["in_combat"] and not move["blocked"] and not move["evading"]
            purpose = role["movement_purpose"]
            failure = purpose.startswith("RETURN_") and purpose != "RETURN_HOME"
            recovery = role.get("return_recovery")
            if isinstance(recovery, dict):
                failure = failure or (type(recovery.get("failures")) is int and recovery["failures"] > 0
                                      and str(recovery.get("failure", "")).startswith("RETURN_"))
            if failure or purpose == "RETURN_HOME":
                self.observed["return"].add(aid)
            returning = calm and move["home_distance"] > RADII[role["role"]] + 2 and role["phase"] in {"IDLE", "ACTING", "MOVING"}
            self.episode("return", agent, row, returning, stationary=True, start=failure)
            motion = calm and role["phase"] == "MOVING" and purpose in LOCAL_MOVES
            if motion:
                self.observed["motion"].add(aid)
            self.episode("motion", agent, row, motion, stationary=True)
            stock = calm and role.get("extensions_enabled") is True and role["role"] not in {"PREDATOR", "PREY"} and economy["food"] > 0
            if stock:
                self.observed["stock_hunger"].add(aid)
            self.episode("stock_hunger", agent, row, stock and needs["hunger"] >= 0.95)
            food_worker = (calm and role.get("extensions_enabled") is True and role["role"] == "WORKER"
                           and agent.get("entry") != 1975 and isinstance(agent.get("home"), dict)
                           and isinstance(agent.get("work"), dict))
            if food_worker:
                self.observed["empty_stock"].add(aid)
            self.episode("empty_stock", agent, row, food_worker and economy["food"] == 0 and needs["hunger"] >= 0.95)
            predator = in_scope and role["role"] == "PREDATOR"
            if predator:
                self.observed["predator_hunger"].add(aid)
            self.episode("predator_hunger", agent, row, predator and needs["hunger"] >= 0.95)
        # A disappeared, dead or abstract agent cannot carry an episode over a
        # respawn/grid reload. Retained spawn positions are never movement data.
        for identity in list(self.runs):
            if identity[0] not in current:
                del self.runs[identity]
        self.inside.intersection_update(current)
        self.previous = current
        self.last_was_fresh = True

    def finish(self, summary: dict, *, integrity_errors: list[str] | None = None) -> dict:
        errors = list(integrity_errors or [])
        if summary.get("status") != "finished" or summary.get("stop_reason") not in {"duration", "stopped"}:
            errors.append("recording_not_completed")
        if summary.get("samples") != self.samples or summary.get("counts") != dict(self.counts):
            errors.append("summary_sample_counts_mismatch")
        elapsed = summary.get("elapsed_seconds")
        if not finite(elapsed) or elapsed <= 0 or (self.last_time is not None and elapsed < self.last_time):
            errors.append("invalid_summary_duration")
        span = elapsed if finite(elapsed) and elapsed > 0 else 0
        fraction = min(1.0, self.fresh_seconds / span) if span else 0
        sufficient = (self.fresh_seconds >= self.policy.minimum_seconds and fraction >= self.policy.fresh_fraction
                      and self.live_samples > 0 and any(self.observed.values()) and not self.quality and not errors)
        findings = sorted(self.worst.values(), key=lambda x: (x["severity"] == "warning", -x["duration_seconds"], x["agent_id"]))
        failed = any(f["severity"] == "failure" for f in findings)
        checks = []
        for key, label in CHECKS.items():
            cases = [f for f in findings if f["check"] == key]
            enough_time = self.fresh_seconds >= getattr(self.policy, key + "_seconds")
            checks.append({"id": key, "label": label, "observed_agents": len(self.observed[key]),
                           "status": ("WARN" if key == "predator_hunger" else "FAIL") if cases else
                           "NOT_OBSERVED" if not self.observed[key] else "PASS" if sufficient and enough_time else "INCONCLUSIVE",
                           "affected_agents": len(cases)})
        complete = sufficient and all(check["status"] != "INCONCLUSIVE" for check in checks)
        status = "FAIL" if failed else "PASS" if complete else "INCONCLUSIVE"
        return {"report_version": 1, "status": status, "exit_code": {"PASS": 0, "FAIL": 3, "INCONCLUSIVE": 2}[status],
                "session_id": self.metadata.get("session_id"), "label": self.metadata.get("label"),
                "build_label": self.metadata.get("build_label", "unknown"), "policy": asdict(self.policy),
                "quality": {"status": "PASS" if sufficient else "INCONCLUSIVE", "samples": self.samples,
                            "counts": dict(self.counts), "continuous_fresh_seconds": round(self.fresh_seconds, 3),
                            "fresh_time_fraction": round(fraction, 5), "live_role_samples": self.live_samples,
                            "problems": dict(self.quality), "integrity_errors": errors},
                "checks": checks, "findings": findings, "roles": self.roles,
                "not_tested": ["Řízené napadení hráčem a pomoc spojenců", "Správnost jednotlivých úderů a animací",
                               "Výkon CPU serveru", "Pilot smečkových vlků"],
                "interpretation": "PASS = v dostatečném záznamu nebylo nalezeno porušení těchto pravidel. "
                                  "Počty přechodů a spotřeby jsou vzorkovaná pozorování, nikoli úplný registr událostí."}


def write_report(report: dict, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    def clean(value):
        return str(value or "—").replace("|", "/").replace("\n", " ").replace("\r", " ")
    lines = ["# Automatický test chování AIWorld", "", f"Výsledek: **{report['status']}**", "",
             f"Session: `{clean(report['session_id'])}`; sestavení: `{clean(report['build_label'])}`.", "",
             report["interpretation"], "", "| Kontrola | Výsledek | Sledovaná NPC | Problémová NPC |",
             "| --- | --- | ---: | ---: |"]
    for check in report["checks"]:
        lines.append(f"| {check['label']} | {check['status']} | {check['observed_agents']} | {check['affected_agents']} |")
    quality = report["quality"]
    lines += ["", f"Kvalita dat: **{quality['status']}**; {quality['samples']} vzorků, "
              f"{quality['continuous_fresh_seconds']:.0f} s souvisle navazujících čerstvých dat "
              f"({quality['fresh_time_fraction']:.1%} délky běhu).",
              f"Problémy dat: `{json.dumps(quality['problems'])}`; integrita: `{json.dumps(quality['integrity_errors'])}`.",
              "", "## Nalezené problémy", "", "Nejdelší epizoda každého NPC a pravidla; úplný seznam je v JSON.", "",
              "| Pravidlo | Spawn | NPC | Délka (s) | Začátek UTC | Konec UTC | Poslední důvod |",
              "| --- | ---: | --- | ---: | --- | --- | --- |"]
    for finding in report["findings"][:100]:
        lines.append("| " + " | ".join(clean(finding[k]) for k in
                     ("check", "spawn_id", "name", "duration_seconds", "start_utc", "end_utc", "movement_purpose")) + " |")
    if not report["findings"]:
        lines += ["", "Žádné překročení prahů. Neověřené scénáře tím nejsou potvrzené."]
    lines += ["", "## Meze testu", "", *["- " + item for item in report["not_tested"]], "",
              "Prahy a souhrny rolí, poklesů hladu, zásob a přechodů lovu jsou v behavior-report.json.", ""]
    for filename, content in (("behavior-report.json", json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"),
                              ("behavior-report.md", "\n".join(lines))):
        temporary = output / (filename + ".tmp")
        temporary.write_text(content, encoding="utf-8")
        temporary.replace(output / filename)


def strict_json(text):
    def reject(value):
        raise ValueError("non-finite JSON number")
    return json.loads(text, parse_constant=reject)


def analyze(directory: Path, policy: Policy | None = None) -> dict:
    summary = strict_json((directory / "summary.json").read_text(encoding="utf-8"))
    if not isinstance(summary, dict) or summary.get("format_version") != 1 or not summary.get("session_id"):
        raise ValueError("unsupported recording summary")
    evaluator = Evaluator(summary, policy)
    errors = []
    parts = summary.get("parts")
    if not isinstance(parts, list) or not parts:
        raise ValueError("recording has no parts")
    footer = None
    for index, name in enumerate(parts, 1):
        if name != f"part-{index:04d}.jsonl.gz" or (directory / name).resolve().parent != directory.resolve():
            errors.append("invalid_part_path_or_order")
            break
        try:
            with gzip.open(directory / name, "rt", encoding="utf-8") as stream:
                header = strict_json(stream.readline(64 * 1024 * 1024 + 1))
                if (header.get("kind") != "session" or header.get("session_id") != summary["session_id"]
                        or header.get("part") != index or header.get("format_version") != 1):
                    raise ValueError("part header mismatch")
                while True:
                    line = stream.readline(64 * 1024 * 1024 + 1)
                    if not line:
                        break  # reaching EOF also verifies the gzip CRC
                    if len(line) > 64 * 1024 * 1024:
                        raise ValueError("oversized recording line")
                    row = strict_json(line)
                    if not isinstance(row, dict) or footer is not None:
                        raise ValueError("invalid record order")
                    if row.get("kind") == "sample":
                        evaluator.observe(row)
                    elif row.get("kind") == "summary" and row.get("session_id", summary["session_id"]) == summary["session_id"]:
                        footer = row
                    else:
                        raise ValueError("unknown record kind")
        except (OSError, EOFError, ValueError, AttributeError, TypeError):
            errors.append(f"unreadable_or_invalid_part:{name}")
            evaluator.break_continuity()
            break
    if (footer is None or any(footer.get(key) != summary.get(key) for key in
                             ("samples", "counts", "status", "stop_reason", "elapsed_seconds"))):
        errors.append("missing_or_mismatched_final_summary")
    return evaluator.finish(summary, integrity_errors=errors)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", type=Path, help="report directory; defaults to the session")
    parser.add_argument("--minimum-minutes", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.minimum_minutes) or args.minimum_minutes <= 0:
        parser.error("--minimum-minutes must be positive and finite")
    try:
        report = analyze(args.session, Policy(minimum_seconds=args.minimum_minutes * 60))
        write_report(report, args.output or args.session)
    except (OSError, ValueError, TypeError) as error:
        print(f"Cannot evaluate recording: {type(error).__name__}", file=sys.stderr)
        return 2
    print(json.dumps({"status": report["status"], "session_id": report["session_id"],
                      "findings": len(report["findings"]), "output": str(args.output or args.session)}))
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
