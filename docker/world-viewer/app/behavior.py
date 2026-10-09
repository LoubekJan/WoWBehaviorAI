"""Read-only acceptance checks for Observer recordings; no gameplay commands."""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass, replace
from copy import copy
import gzip
import json
import math
from pathlib import Path
import sys

if __package__:
    from .scope import contains_position, normalize_scope
else:
    from scope import contains_position, normalize_scope


@dataclass(frozen=True)
class Policy:
    minimum_seconds: float = 3600
    fresh_fraction: float = 0.95
    return_seconds: float = 300
    return_duration_seconds: float = 600
    physical_stall_seconds: float = 300
    motion_seconds: float = 60
    outside_seconds: float = 30
    stock_hunger_seconds: float = 600
    empty_stock_seconds: float = 600
    predator_hunger_seconds: float = 1800
    prey_hunger_seconds: float = 600
    prey_threat_hunger_seconds: float = 600
    advice_wait_seconds: float = 600
    position_tolerance: float = 1
    planning_deferred_seconds: float = 300


CHECKS = {
    "return": "Návrat bez dlouhého zablokování",
    "return_duration": "Dokončení návratu i při pohybu",
    "physical_stall": "Skutečné zaseknutí napříč změnami chování",
    "motion": "Pohybový úkol se skutečným posunem",
    "outside": "Zachování řízení v oblasti simulace",
    "stock_hunger": "Jídlo při hladu a dostupných zásobách",
    "empty_stock": "Obnovení prázdné zásoby jídla u pracovní rutiny",
    "predator_hunger": "Dlouhodobý hlad predátorů (upozornění)",
    "prey_hunger": "Dlouhodobý hlad kořisti v klidu",
    "prey_threat_hunger": "Dlouhodobý hlad kořisti při opakovaném nebezpečí (upozornění)",
    "advice_wait": "Obsluha způsobilých NPC ve frontě AI",
    "planning_deferred": "Dlouhodobě odkládané rozhodování",
}
WARNING_CHECKS = {"predator_hunger", "prey_threat_hunger"}
RADII = {"PREDATOR": 12, "PREY": 6, "GUARD": 8, "COMBATANT": 10,
         "CIVILIAN": 4, "WORKER": 4, "TRAVELER": 12, "SERVICE": 0}
SCOPED = {"READY", "ACTIVE", "CURATED_ROUTINE", "GROUP_ACTIVITY"}
LOCAL_MOVES = {"RETURN_HOME", "FORAGE_SEARCH", "LOCAL_ROAM", "HERD_COHESION", "PATROL_COMPANION", "FOOD_SUPPLY", "LOCAL_RECOVERY_MOVE"}
CARE_PURPOSES = {"NEEDS_CARE", "REFUGE_CARE", "LOCAL_RECOVERY_CARE"}
CARE_SETTLE_SECONDS = 15  # bounded ordinary idle pause after a sampled care action


def finite(value) -> bool:
    return type(value) in (int, float) and math.isfinite(value)


class Evaluator:
    """Keep only adjacent observations, open episodes and each agent's worst case."""

    def __init__(self, metadata: dict, policy: Policy | None = None):
        self.metadata = metadata
        self.scope = normalize_scope(metadata.get("scope"))
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
        self.advice = {}
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

    def episode(self, key, agent, row, condition, *, stationary=False, start=True, paused=False, minimum_seconds=0):
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
        if key in {"physical_stall", "planning_deferred"}:
            # Physical stalls suspend during combat/root/evade; planning waits
            # suspend only during explicit care. Neither pause proves movement.
            previous = run.get("last_seconds", row["elapsed_seconds"])
            if paused or run.get("paused", False):
                run["paused_seconds"] = run.get("paused_seconds", 0) + row["elapsed_seconds"] - previous
            run["last_seconds"] = row["elapsed_seconds"]
            run["paused"] = paused
            if paused:
                return
        duration = row["elapsed_seconds"] - run["start_seconds"]
        if key in {"physical_stall", "planning_deferred"}:
            duration -= run.get("paused_seconds", 0)
        limit = getattr(self.policy, key + "_seconds" if key != "return" else "return_seconds")
        if duration < max(limit, minimum_seconds):
            return
        detail = {**run, "check": key, "duration_seconds": round(duration, 3),
                  "end_utc": row.get("recorded_at_utc"), "end_seconds": row["elapsed_seconds"],
                  "movement_purpose": agent["living_role"]["movement_purpose"],
                  "home_distance": agent["movement"]["home_distance"],
                  "severity": "warning" if key in WARNING_CHECKS else "failure"}
        if key == "advice_wait":
            detail["advice"] = agent["living_role"]["advice"]
        if key == "planning_deferred":
            advice = agent["living_role"].get("advice")
            detail["lifetime_ms"] = advice.get("lifetime_ms") if isinstance(advice, dict) else None
            planning = agent["living_role"].get("planning")
            if isinstance(planning, dict):
                detail["planning"] = planning
        recovery = agent["living_role"].get("return_recovery")
        if isinstance(recovery, dict):
            detail["return_recovery"] = recovery
        forage = agent["living_role"].get("forage")
        if key == "predator_hunger" and isinstance(forage, dict):
            detail["forage"] = forage
            scan = forage.get("scanned_at_ms")
            capture = row["state"]["captured_at_ms"]
            detail["food_observation"] = (
                "STALE_SCAN" if type(scan) is not int or not 0 < scan <= capture or capture-scan > 60000 else
                "NO_LOCAL_PREY" if forage.get("nearby_prey") == 0 else
                "PREY_NOT_ATTACKABLE" if forage.get("attackable_prey") == 0 else
                "NO_REACHABLE_PREY" if forage.get("reachable_prey") == 0 else "REACHABLE_PREY_SEEN")
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
        if "scope" in state:
            try:
                scope_matches = normalize_scope(state["scope"]) == self.scope
            except ValueError:
                scope_matches = False
            if not scope_matches:
                self.quality["scope_mismatch"] += 1
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
            if (role.get("role") not in RADII or role.get("status") not in SCOPED | {"OUTSIDE_ELWYNN", "OUTSIDE_SCOPE"}):
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
            advice = role.get("advice")
            if isinstance(advice, dict) and type(advice.get("lifetime_ms")) is int:
                key = f"{aid}:{advice['lifetime_ms']}"
                stats = self.advice.setdefault(key, {"agent_id": aid, "spawn_id": agent["spawn_id"],
                    "lifetime_ms": advice["lifetime_ms"], "counters": {}})
                stats["enabled"] = advice.get("enabled") is True
                stats["last_status"] = str(advice.get("status", "UNKNOWN"))[:80]
                for metric in ("requests", "selected", "started", "arrived", "home_success", "food_success", "rejected", "unavailable", "reused"):
                    value = advice.get(metric)
                    if type(value) is int and value >= 0:
                        stats["counters"][metric] = max(value, stats["counters"].get(metric, 0))
            old = self.previous.get(aid)
            old_advice = old['living_role'].get('advice') if old else None
            lifetime_changed = (isinstance(old_advice, dict) and isinstance(advice, dict)
                                and old_advice.get('lifetime_ms') != advice.get('lifetime_ms'))
            if old and (old["spawn_id"] != agent["spawn_id"] or old["position"]["map_id"] != pos["map_id"]
                        or old["living_role"]["role"] != role["role"]
                        or lifetime_changed):
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
            in_scope = role["status"] in SCOPED and contains_position(self.scope, pos)
            if in_scope:
                self.inside.add(aid)
                self.observed["outside"].add(aid)
            outside = aid in self.inside and (role["status"] in {"OUTSIDE_ELWYNN", "OUTSIDE_SCOPE"}
                                               or not contains_position(self.scope, pos))
            self.episode("outside", agent, row, outside)
            calm = in_scope and not agent["in_combat"] and not move["blocked"] and not move["evading"]
            purpose = role["movement_purpose"]
            local_recovery = purpose in {"LOCAL_RECOVERY_MOVE", "LOCAL_RECOVERY_CARE"}
            failure = purpose.startswith("RETURN_") and purpose != "RETURN_HOME"
            recovery = role.get("return_recovery")
            if isinstance(recovery, dict):
                failure = failure or (type(recovery.get("failures")) is int and recovery["failures"] > 0
                                      and str(recovery.get("failure", "")).startswith("RETURN_"))
            if failure or purpose == "RETURN_HOME" or local_recovery:
                self.observed["return"].add(aid)
            returning = calm and move["home_distance"] > RADII[role["role"]] + 2 and role["phase"] in {"IDLE", "ACTING", "MOVING"}
            self.episode("return", agent, row, returning, stationary=True, start=failure)
            care = (role["phase"] == "ACTING" and role["activity"] in {"REST", "LOOK"}
                    and (purpose in CARE_PURPOSES or purpose.startswith("RETURN_")
                         or purpose == "PLANNING_DEFERRED"))
            # A moving A->B->A loop resets the stationary test forever. Once
            # a return is observed, time the whole attempt until home, an
            # interruption or a different activity; animations/idle persist.
            deferred_return = purpose == "PLANNING_DEFERRED" and isinstance(recovery, dict)
            return_care = care and (isinstance(recovery, dict) or (aid, "return_duration") in self.runs)
            return_attempt = (returning and role["status"] in {"READY", "ACTIVE"}
                              and (role["phase"] != "ACTING" or role["activity"] in {"NONE", "REST", "LOOK"})
                              and (purpose in {"NONE", "PLANNING_DEFERRED"}
                                   or purpose.startswith("RETURN_") or local_recovery or return_care))
            return_start = failure or purpose == "RETURN_HOME" or local_recovery or deferred_return
            if return_attempt and return_start:
                self.observed["return_duration"].add(aid)
            self.episode("return_duration", agent, row, return_attempt, start=return_start)
            # A pending decision can starve even at the exact home position.
            # Seed only from the explicit yield purpose, not ordinary idle.
            # Care preserves the wait but spends none of its failure budget;
            # eating/hunting/other actions and observed feeding reset it.
            planning_run = self.runs.get((aid, "planning_deferred"))
            feeding_progress = old is not None and old["needs"]["hunger"] - needs["hunger"] > 0.1
            planning_scope = (calm and role["status"] in {"READY", "ACTIVE"}
                              and role["phase"] in {"IDLE", "ACTING"} and not feeding_progress)
            deferred = (purpose == "PLANNING_DEFERRED" and not care
                        and (role["phase"] == "IDLE" or role["activity"] == "NONE"))
            settling = (planning_run is not None and role["phase"] == "IDLE" and purpose == "NONE"
                        and 0 <= t - planning_run.get("care_last_seconds", -math.inf) <= CARE_SETTLE_SECONDS)
            if planning_scope and care and planning_run is not None:
                planning_run["care_last_seconds"] = t
            planning_wait = planning_scope and (deferred or care or settling)
            if planning_scope and deferred:
                self.observed["planning_deferred"].add(aid)
            self.episode("planning_deferred", agent, row, planning_wait, stationary=True,
                         start=deferred, paused=care or settling)
            # A phase flip (especially repeated fleeing -> idle) cannot hide
            # an immobile NPC. Seed from a movement/return attempt, not from
            # standing workers, service NPCs or ordinary resting wildlife.
            physical = (in_scope and role["status"] in {"READY", "ACTIVE"}
                        and move["home_distance"] > RADII[role["role"]] + 2)
            physical_start = calm and (failure or purpose in LOCAL_MOVES
                                        or purpose in {"BOUNDED_ESCAPE", "AWAY_FROM_DANGER", "CHECK_ALLY_ALARM",
                                                       "ESCAPE_NAV_REJOIN", "ESCAPE_NO_PATH"})
            if physical and physical_start:
                self.observed["physical_stall"].add(aid)
            self.episode("physical_stall", agent, row, physical, stationary=True,
                         start=physical_start, paused=not calm)
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
            danger_remaining = role.get("danger_remaining_ms", 0)
            danger = ((finite(danger_remaining) and danger_remaining > 0) or
                      str(role.get("awareness", "QUIET")) in {"PREDATOR_SEEN", "DIRECT_THREAT", "REMEMBERED_DANGER",
                          "HOSTILE_APPROACH", "HERD_ALARM", "ALLY_IN_DANGER"} or
                      role["phase"] in {"FLEEING", "SEEKING_SAFETY"})
            prey = calm and not danger and role["role"] == "PREY" and role["phase"] in {"IDLE", "ACTING", "MOVING"}
            if prey:
                self.observed["prey_hunger"].add(aid)
            self.episode("prey_hunger", agent, row, prey and needs["hunger"] >= 0.95)
            threatened_prey = in_scope and danger and role["role"] == "PREY"
            if threatened_prey:
                self.observed["prey_threat_hunger"].add(aid)
            self.episode("prey_threat_hunger", agent, row, threatened_prey and needs["hunger"] >= 0.95)
            queue_wait = advice.get("queue_wait_ms") if isinstance(advice, dict) else None
            queue_size = advice.get("queue_size", 0) if isinstance(advice, dict) else 0
            queued = (calm and type(queue_wait) is int and queue_wait >= 0 and type(queue_size) is int and
                      0 < queue_size <= 2048 and advice.get("queue_dispatchable") is True)
            if queued:
                self.observed["advice_wait"].add(aid)
                previous_wait = old_advice.get("queue_wait_ms") if isinstance(old_advice, dict) else None
                if type(previous_wait) is int and queue_wait < previous_wait:
                    self.runs.pop((aid, "advice_wait"), None)  # new ticket between samples
            # Include the 3:1 return/food priority and update cadence. A busy
            # queue is not itself a fault; a stuck eligible ticket beyond a
            # generous full service round is.
            self.episode("advice_wait", agent, row, queued, minimum_seconds=queue_size * 10 if queued else 0)
        # A disappeared, dead or abstract agent cannot carry an episode over a
        # respawn/grid reload. Retained spawn positions are never movement data.
        for identity in list(self.runs):
            if identity[0] not in current:
                del self.runs[identity]
        self.inside.intersection_update(current)
        self.previous = current
        self.last_was_fresh = True

    def checkpoint(self, summary: dict, minimum_seconds: float = 900) -> dict:
        """Read-only view of navigation checks; keep the four-hour state intact."""
        early = copy(self)
        early.policy = replace(self.policy, minimum_seconds=minimum_seconds)
        report = early.finish(summary, partial=True)
        selected = {"return", "return_duration", "physical_stall", "motion", "outside", "advice_wait", "planning_deferred"}
        report["checks"] = [c for c in report["checks"] if c["id"] in selected]
        report["findings"] = [f for f in report["findings"] if f["check"] in selected]
        report["scope"] = "early_navigation"
        report["recording_complete"] = False
        report["status"] = ("FAIL" if report["findings"] else "PASS" if
            report["quality"]["status"] == "PASS" and all(c["status"] in {"PASS", "NOT_OBSERVED"} for c in report["checks"])
            else "INCONCLUSIVE")
        report["exit_code"] = {"PASS": 0, "FAIL": 3, "INCONCLUSIVE": 2}[report["status"]]
        report["not_tested"] = [*report["not_tested"], "Dlouhodobý hlad a pozdější část pokračujícího běhu"]
        return report

    def finish(self, summary: dict, *, integrity_errors: list[str] | None = None, partial: bool = False) -> dict:
        errors = list(integrity_errors or [])
        if not (partial and summary.get("status") == "running") and (
                summary.get("status") != "finished" or summary.get("stop_reason") not in {"duration", "stopped"}):
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
                           "status": ("WARN" if key in WARNING_CHECKS else "FAIL") if cases else
                           "NOT_OBSERVED" if not self.observed[key] else "PASS" if sufficient and enough_time else "INCONCLUSIVE",
                           "affected_agents": len(cases)})
        complete = sufficient and all(check["status"] != "INCONCLUSIVE" for check in checks)
        status = "FAIL" if failed else "PASS" if complete else "INCONCLUSIVE"
        return {"report_version": 1, "status": status, "exit_code": {"PASS": 0, "FAIL": 3, "INCONCLUSIVE": 2}[status],
                "session_id": self.metadata.get("session_id"), "label": self.metadata.get("label"),
                "simulation_scope": self.scope,
                "build_label": self.metadata.get("build_label", "unknown"), "policy": asdict(self.policy),
                "quality": {"status": "PASS" if sufficient else "INCONCLUSIVE", "samples": self.samples,
                            "counts": dict(self.counts), "continuous_fresh_seconds": round(self.fresh_seconds, 3),
                            "fresh_time_fraction": round(fraction, 5), "live_role_samples": self.live_samples,
                            "problems": dict(self.quality), "integrity_errors": errors},
                "checks": checks, "findings": findings, "roles": self.roles,
                "recovery_advice": list(self.advice.values()),
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
             f"Oblast: {clean(report['simulation_scope']['name'])}; mapa {report['simulation_scope']['map_id']}; "
             f"zóny {', '.join(map(str, report['simulation_scope']['zone_ids']))}.", "",
             report["interpretation"], "", "| Kontrola | Výsledek | Sledovaná NPC | Problémová NPC |",
             "| --- | --- | ---: | ---: |"]
    for check in report["checks"]:
        lines.append(f"| {check['label']} | {check['status']} | {check['observed_agents']} | {check['affected_agents']} |")
    quality = report["quality"]
    if report.get("scope") == "early_navigation":
        lines += ["", "Průběžná kontrola navigace. Záznam pokračuje; tento výsledek nenahrazuje závěrečný čtyřhodinový test."]
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
    food = [f for f in report['findings'] if f.get('food_observation')]
    if food:
        lines += ["", "## Pozorování potravy u hladových predátorů", "",
            "Poslední místní pozorování, nikoli důkaz, že v celém lovišti není potrava. "
            "STALE_SCAN znamená chybějící či starší než minutový průzkum. Podrobnosti cesty jsou v JSON.", "",
            "| Spawn | Pozorování | Poslední výsledek hledání cesty |", "| ---: | --- | --- |"]
        for f in food:
            lines.append(f"| {f['spawn_id']} | {clean(f['food_observation'])} | "
                         f"{clean(f['forage'].get('navigation', {}).get('failure', 'UNKNOWN'))} |")
    advice = report.get("recovery_advice", [])
    if advice:
        active_advice = [item for item in advice if any(item['counters'].values()) or item['last_status'] != 'IDLE']
        active_advice.sort(key=lambda item: (-item['counters'].get('requests', 0), item['spawn_id']))
        lines += ["", "## Pomoc lokální AI", "",
            f"Přístup k AI byl zaznamenán u {len({item['agent_id'] for item in advice if item.get('enabled')})} NPC. "
            f"Aktivitu nebo čekání má {len(active_advice)} materializací; tabulka uvádí nejvýše 100, úplná data jsou v JSON.", "",
            "Čítače jsou maxima za každou materializaci NPC. Přijatý návrh ani spuštěný krok není dokončený návrat.", "",
            "| Spawn | Požadavky | Vybráno AI | Z paměti | Spuštěno | Dosažený krok | Návrat domů | Nalezené jídlo | Zamítnuto | Chyby AI | Poslední stav |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"]
        for item in active_advice[:100]:
            c = item["counters"]
            values = [item["spawn_id"], *[c.get(k, 0) for k in ("requests", "selected", "reused", "started", "arrived", "home_success", "food_success", "rejected", "unavailable")], item["last_status"]]
            lines.append("| " + " | ".join(str(v).replace("|", "/").replace("\n", " ").replace("\r", " ") for v in values) + " |")
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
    parser.add_argument("--planning-deferred-seconds", type=float, default=Policy().planning_deferred_seconds,
                        help="maximum observed decision deferral time, excluding stationary care (default: 300)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.minimum_minutes) or args.minimum_minutes <= 0:
        parser.error("--minimum-minutes must be positive and finite")
    if not math.isfinite(args.planning_deferred_seconds) or args.planning_deferred_seconds <= 0:
        parser.error("--planning-deferred-seconds must be positive and finite")
    try:
        report = analyze(args.session, Policy(minimum_seconds=args.minimum_minutes * 60,
                         planning_deferred_seconds=args.planning_deferred_seconds))
        write_report(report, args.output or args.session)
    except (OSError, ValueError, TypeError) as error:
        print(f"Cannot evaluate recording: {type(error).__name__}", file=sys.stderr)
        return 2
    print(json.dumps({"status": report["status"], "session_id": report["session_id"],
                      "findings": len(report["findings"]), "output": str(args.output or args.session)}))
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
