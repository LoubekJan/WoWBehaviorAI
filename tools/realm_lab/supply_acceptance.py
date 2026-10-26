"""Read-only physical acceptance of supply-v8 recorder/API observations.

PASS needs three continuous observed hunt/harvest/carry/depot-delivery cycles
and one observed depot meal in a common uninterrupted ledger epoch. Counters,
phase labels, spawn fallbacks and starting balances alone are never evidence.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import json
import math
from pathlib import Path
from typing import Iterable

try:
    from . import supply_population as population, wildlife_acceptance as wildlife
except ImportError:
    import supply_population as population
    import wildlife_acceptance as wildlife

HUNTER_IDS = population.HUNTER_IDS
OWNED_IDS = population.SPAWN_IDS
PREY_IDS = wildlife.PREY_IDS
PREDATOR_IDS = wildlife.PREDATOR_IDS
EXIT_CODES = {"PASS": 0, "INCONCLUSIVE": 2, "FAIL": 3}
PHASES = ("HUNTING", "TARGET_DEATH", "HARVESTING", "HARVESTED", "RETURNING_TO_DEPOT", "DELIVERING", "DELIVERED")
COUNTERS = ("harvested", "delivered", "consumed", "lost")
ACCOUNTING = ("initial_food", "harvested", "in_depots", "in_cargo", "consumed", "lost")
finite, point, distance, positive_int = wildlife.finite, wildlife.point, wildlife.distance, wildlife.positive_int
recording_rows = wildlife.recording_rows


def counter(value) -> int | None:
    if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or len(value) > 20:
        return None
    result = int(value)
    return result if result <= 2**64 - 1 and str(result) == value else None


def quantity(value, maximum=2**32 - 1) -> bool:
    return type(value) is int and 0 <= value <= maximum


def dist3(a, b) -> float:
    return math.dist(a, b)


@dataclass
class Track:
    previous: dict | None = None
    anchor: tuple | None = None
    since: float | None = None
    maximum_displacement: float = 0
    continuous_seconds: float = 0
    samples: int = 0
    movements: dict = field(default_factory=dict)
    cycle: dict | None = None
    partial: dict | None = None
    cycles: list = field(default_factory=list)
    meals: list = field(default_factory=list)
    interruptions: Counter = field(default_factory=Counter)


class _Evaluator:
    def __init__(self, minimum_seconds=900, max_gap_seconds=5, *, consumers=None):
        if not finite(minimum_seconds) or minimum_seconds <= 0 or not finite(max_gap_seconds) or max_gap_seconds <= 0:
            raise ValueError("observation windows must be finite and positive")
        manifest = json.loads(wildlife.MANIFEST.read_text(encoding="utf-8"))
        self.actors = {a["spawn_id"]: a for a in manifest["actors"]}
        self.bounds = manifest["bounds"]
        self.depots = {d["id"]: d["position"] for d in population.DEPOTS}
        self.grounds = {g["id"]: g["position"] for g in population.GROUNDS}
        # The default gate remains the reviewed 23-owner supply profile. The
        # consumer evaluator supplies its separately reviewed, fixed roster.
        self.consumers = consumers or {}
        self.owned_ids = OWNED_IDS | self.consumers.keys()
        self.depot_members = {1: (901034,), 2: (901061, 901062)}
        for sid, spec in self.consumers.items():
            self.depot_members[spec["depot"]] += (sid,)
        self.tracks = {sid: Track() for sid in sorted(HUNTER_IDS)}
        self.minimum_seconds, self.max_gap = minimum_seconds, max_gap_seconds
        self.quality = Counter()
        self.violations = {}
        self.samples = self.fresh = self.population_frames = 0
        self.epoch = self.direct_sequence = 0
        self.session = self.previous_session = self.last_sequence = self.last_time = None
        self.last_capture = self.last_payload = self.last_unique_time = None
        self.previous_rows, self.previous_prey, self.previous_sources = {}, {}, {}
        self.previous_accounting = self.latest_accounting = None
        self.accounting_by_epoch = {}

    def violation(self, code, sid=0):
        self.violations[(code, sid)] = {"code": code, "spawn_id": sid}

    def interrupt(self, sid, reason):
        track = self.tracks[sid]
        if track.previous is not None and track.cycle is None:
            track.interruptions[reason] += 1
        self.archive_cycle(sid, reason)
        track.previous = track.anchor = track.since = None

    def archive_cycle(self, sid, reason):
        track = self.tracks[sid]
        if track.cycle is not None:
            partial = {**track.cycle, "interrupted_by": reason}
            if track.partial is None or len(partial["covered"]) > len(track.partial["covered"]):
                track.partial = partial
            track.interruptions[reason] += 1
        track.cycle = None

    def reset(self, reason):
        self.epoch += 1
        self.quality[reason] += 1
        for sid in self.tracks:
            self.interrupt(sid, reason)
        self.previous_rows, self.previous_prey, self.previous_sources = {}, {}, {}
        self.previous_accounting = None

    def usable(self, record):
        if not isinstance(record, dict):
            self.reset("invalid_record")
            return None
        if record.get("kind") == "session":
            self.session = record.get("session_id")
            return None
        if record.get("kind") == "summary":
            return None
        direct = "state" not in record and "agents" in record
        if direct:
            self.direct_sequence += 1
            capture = record.get("captured_at_ms")
            record = dict(state=record, status="fresh", sequence=self.direct_sequence,
                          elapsed_seconds=capture / 1000 if finite(capture) else None)
        self.samples += 1
        state, t, sequence = record.get("state"), record.get("elapsed_seconds"), record.get("sequence")
        if not isinstance(state, dict) or not finite(t) or t < 0 or not positive_int(sequence):
            self.reset("invalid_sample")
            return None
        session, capture = record.get("session_id", self.session), state.get("captured_at_ms")
        same_capture = self.last_capture is not None and capture == self.last_capture
        continuous = self.last_time is None or (session == self.previous_session and sequence == self.last_sequence + 1 and
            (0 < t - self.last_time <= self.max_gap or direct and same_capture and t == self.last_time))
        if not continuous:
            self.reset("session_sequence_or_time_gap")
        self.previous_session, self.last_time, self.last_sequence = session, t, sequence
        if (record.get("status") != "fresh" or state.get("configured") is not True or state.get("stale") is not False or
            state.get("version") != 4 or not positive_int(capture) or type(state.get("age_ms")) is not int or
            not 0 <= state["age_ms"] < 5000 or not isinstance(state.get("agents"), list)):
            self.reset("stale_or_unusable_snapshot")
            return None
        try:
            payload = json.dumps({k: state.get(k) for k in ("version", "scope", "agents")},
                                 sort_keys=True, separators=(",", ":"), allow_nan=False)
        except (ValueError, TypeError):
            self.reset("invalid_payload")
            return None
        if same_capture:
            if continuous and payload == self.last_payload and self.last_unique_time is not None and 0 <= t - self.last_unique_time <= self.max_gap:
                self.quality["identical_capture_ignored"] += 1
                return None
            self.reset("mutated_repeat_or_duplicate_gap")
            return None
        if self.last_capture is not None and (not 0 < capture - self.last_capture <= self.max_gap * 1000 or
            self.last_unique_time is not None and not 0 < t - self.last_unique_time <= self.max_gap):
            self.reset("capture_reset_or_gap")
        self.last_capture, self.last_payload, self.last_unique_time = capture, payload, t
        return state, t, capture

    def validate_population(self, agents):
        if any(not isinstance(a, dict) for a in agents):
            self.reset("invalid_population")
            return None
        ids = [a.get("spawn_id") for a in agents]
        agent_ids = [a.get("agent_id") for a in agents]
        if any(not positive_int(sid) for sid in ids + agent_ids) or len(set(ids)) != len(ids) or len(set(agent_ids)) != len(agent_ids):
            self.reset("invalid_or_duplicate_identity")
            return None
        by_id = {a["spawn_id"]: a for a in agents}
        for sid in set(ids) - self.actors.keys():
            self.violation("foreign_spawn_in_population", sid)
        complete = set(ids) == self.actors.keys()
        if not complete:
            self.quality["incomplete_100_actor_population"] += 1
        for sid, actor in by_id.items():
            if sid not in self.actors:
                continue
            owned = sid in self.owned_ids
            if actor.get("agent_id") != sid:
                self.violation("agent_spawn_identity_mismatch", sid)
            if actor.get("native_creature") is not (not owned) or actor.get("control_mode") != ("AI_WORLD_CONTROLLED" if owned else "OBSERVE_ONLY"):
                self.violation("ownership_changed", sid)
            if actor.get("entry") != self.actors[sid]["entry"]:
                self.violation("entry_changed", sid)
            home = actor.get("home")
            if point(home) is None:
                self.quality["missing_home"] += 1
                complete = False
            elif home.get("map_id") != 725 or any(abs(home[k] - self.actors[sid]["home"][k]) > 0.01 for k in ("x", "y", "z")):
                self.violation("home_changed", sid)
            if not owned:
                if actor.get("supply") is not None or actor.get("needs") is not None or actor.get("living_role") is not None:
                    self.violation("native_acquired_ai_state", sid)
            else:
                expected_type = self.consumers[sid]["type"] if sid in self.consumers else "COMBATANT" if sid in HUNTER_IDS else "PREDATOR" if sid in PREDATOR_IDS else "PREY"
                faction = self.consumers[sid]["faction"] if sid in self.consumers else (1 if sid == 901034 else 3) if sid in HUNTER_IDS else 0
                if actor.get("type") != expected_type or actor.get("world_faction") != faction:
                    self.violation("role_or_faction_changed", sid)
                pos = actor.get("position") or {}
                if not isinstance(pos, dict):
                    self.quality["invalid_position"] += 1
                    continue
                xyz = point(pos)
                if xyz is not None and (pos.get("map_id") != 725 or not (self.bounds["min_x"] <= xyz[0] <= self.bounds["max_x"] and
                    self.bounds["min_y"] <= xyz[1] <= self.bounds["max_y"])):
                    self.violation("outside_map_or_bounds", sid)
        if complete:
            self.population_frames += 1
        return by_id

    def row(self, actor, t, capture):
        sid = actor["spawn_id"]
        pos = actor.get("position") or {}
        xyz, role, needs, movement = point(pos), actor.get("living_role") or {}, actor.get("needs") or {}, actor.get("movement") or {}
        if not all(isinstance(value, dict) for value in (pos, role, needs, movement)):
            self.quality["invalid_live_observation"] += 1
            return None
        if pos.get("source") != "live" or xyz is None or actor.get("world_state") != "MATERIALIZED":
            self.quality["spawn_or_last_known_fallback"] += 1
            return None
        alive, guid = actor.get("alive"), actor.get("runtime_guid")
        if (type(alive) is not bool or not isinstance(guid, str) or not guid.strip() or not finite(actor.get("health")) or
            (actor["health"] <= 0 if alive else actor["health"] != 0)):
            self.quality["missing_live_identity_or_health"] += 1
            return None
        life = role.get("lifetime_ms")
        if alive and (not positive_int(life) or life > capture or role.get("enabled") is not True):
            self.quality["missing_role_lifetime"] += 1
            return None
        if point(actor.get("home")) is None:
            return None
        if alive and sid in HUNTER_IDS and (role.get("role") != "HUNTER" or not finite(needs.get("hunger")) or
            not 0 <= needs["hunger"] <= 1 or not finite(movement.get("home_distance"))):
            self.quality["missing_hunter_role_or_needs"] += 1
            return None
        if alive and sid in HUNTER_IDS and abs(movement["home_distance"] - distance(xyz, point(actor["home"]))) > 1:
            self.violation("runtime_home_changed", sid)
            return None
        result = dict(agent=actor, position=xyz, role=role, alive=alive, guid=guid, life=life,
                      t=t, capture=capture, epoch=self.epoch, hunger=needs.get("hunger"), movement=movement)
        if sid not in HUNTER_IDS | PREDATOR_IDS | self.consumers.keys():
            return result
        supply = actor.get("supply")
        if not isinstance(supply, dict) or supply.get("enabled") is not True:
            self.quality["missing_supply"] += 1
            return None
        counters = {name: counter(supply.get(name)) for name in COUNTERS}
        if any(value is None for value in counters.values()) or not quantity(supply.get("cargo"), 6) or not quantity(supply.get("capacity"), 6) or supply["cargo"] > supply["capacity"]:
            self.violation("invalid_supply_quantity", sid)
            return None
        result.update(supply=supply, counters=counters, cargo=supply["cargo"], phase=supply.get("phase"), source=None, remaining=None)
        source, incarnation = counter(supply.get("carcass_spawn_id")), counter(supply.get("carcass_life"))
        remaining = supply.get("carcass_remaining")
        if source is not None or incarnation is not None:
            if source not in PREY_IDS or not incarnation or not quantity(remaining, 6):
                self.violation("invalid_carcass_identity_or_quantity", sid)
            else:
                result.update(source=(source, incarnation), remaining=remaining)
        if sid in HUNTER_IDS or sid in self.consumers:
            consumer = sid in self.consumers
            expected = self.consumers[sid]["depot"] if consumer else 1 if sid == 901034 else 2
            depot, ground = supply.get("depot"), supply.get("hunting_ground")
            if supply.get("role") != ("CONSUMER" if consumer else "HUNTER") or supply.get("depot_id") != expected:
                self.violation("hunter_depot_or_capability_changed", sid)
                return None
            if consumer and (supply["cargo"] != 0 or supply["capacity"] != 0 or
                any(counters[key] != 0 for key in ("harvested", "delivered", "lost")) or
                any(supply.get(key) is not None for key in ("hunting_ground", "carcass_remaining", "carcass_spawn_id", "carcass_life"))):
                self.violation("consumer_acquired_hunting_or_cargo_capability", sid)
                return None
            if not isinstance(depot, dict) or point(depot.get("position")) is None or not consumer and point(ground) is None:
                self.quality["missing_depot_or_hunting_ground"] += 1
                return None
            anchors = ((depot["position"], self.depots[expected]),) if consumer else ((depot["position"], self.depots[expected]), (ground, self.grounds[expected]))
            if depot.get("id") != expected or any(p.get("map_id") != 725 or dist3(point(p), point(want)) > 0.05 for p, want in anchors):
                self.violation("depot_or_hunting_ground_moved", sid)
                return None
            if (not quantity(depot.get("stock")) or not positive_int(depot.get("capacity")) or not quantity(depot["capacity"]) or depot["stock"] > depot["capacity"] or
                counter(depot.get("delivered")) is None or counter(depot.get("consumed")) is None):
                self.violation("invalid_depot_quantity", sid)
                return None
            result.update(depot=depot, depot_distance=dist3(xyz, point(depot["position"])))
        return result

    def accounting(self, rows):
        values, depots = [], {}
        for sid, row in rows.items():
            if "supply" not in row:
                continue
            raw = row["supply"].get("accounting")
            if not isinstance(raw, dict) or any(counter(raw.get(k)) is None for k in ACCOUNTING):
                self.quality["missing_accounting"] += 1
                return None
            value = {k: counter(raw[k]) for k in ACCOUNTING}
            if raw.get("conserved") is not True or value["initial_food"] + value["harvested"] != sum(value[k] for k in ("in_depots", "in_cargo", "consumed", "lost")):
                self.violation("food_conservation_broken")
            values.append(value)
            if "depot" in row:
                key, depot = row["depot"]["id"], row["depot"]
                if key in depots and depots[key] != depot:
                    self.violation("conflicting_depot_copies", sid)
                depots[key] = depot
        if not values:
            return None
        if any(value != values[0] for value in values):
            self.violation("conflicting_accounting_copies")
        value = values[0]
        if len(depots) == 2 and sum(d["stock"] for d in depots.values()) != value["in_depots"]:
            self.violation("depot_total_disagrees_with_accounting")
        if HUNTER_IDS.issubset(rows) and sum(rows[sid]["cargo"] for sid in HUNTER_IDS) != value["in_cargo"]:
            self.violation("cargo_total_disagrees_with_accounting")
        if self.previous_accounting and (value["initial_food"] != self.previous_accounting["initial_food"] or
            any(value[k] < self.previous_accounting[k] for k in ("harvested", "consumed", "lost"))):
            self.reset("ledger_restart_or_reset")
        self.latest_accounting = self.previous_accounting = value
        self.accounting_by_epoch[self.epoch] = value
        return value

    def depot_flows(self, rows):
        # Two gnolls can deliver and eat in the same producer tick. Reconcile
        # all owners of one depot instead of pretending the net stock change
        # belongs to whichever actor happens to be checked first.
        for depot_id, members in self.depot_members.items():
            if not all(sid in rows and sid in self.previous_rows and "depot" in rows[sid] and
                       "depot" in self.previous_rows[sid] for sid in members):
                continue
            changes = {key: sum(rows[sid]["counters"][key] - self.previous_rows[sid]["counters"][key]
                                for sid in members) for key in ("delivered", "consumed")}
            if any(value < 0 for value in changes.values()):
                continue
            before, after = self.previous_rows[members[0]]["depot"], rows[members[0]]["depot"]
            if (after["stock"] - before["stock"] != changes["delivered"] - changes["consumed"] or
                counter(after["delivered"]) - counter(before["delivered"]) != changes["delivered"] or
                counter(after["consumed"]) - counter(before["consumed"]) != changes["consumed"]):
                self.violation("depot_stock_change_not_reconciled", depot_id)
                continue
            for sid in members:
                rows[sid]["depot_flow"] = changes

    def observe(self, record):
        usable = self.usable(record)
        if usable is None:
            return
        state, t, capture = usable
        by_id = self.validate_population(state["agents"])
        if by_id is None:
            return
        self.fresh += 1
        rows = {}
        for sid in sorted(self.owned_ids & by_id.keys()):
            row = self.row(by_id[sid], t, capture)
            if row is not None:
                rows[sid] = row
        accounting = self.accounting(rows)
        if accounting is None:
            self.reset("no_complete_accounting")
            return
        for row in rows.values():
            row["epoch"] = self.epoch
        self.depot_flows(rows)
        prey = {sid: row for sid, row in rows.items() if sid in PREY_IDS}
        sources, extraction = {}, Counter()
        for sid, row in rows.items():
            if row.get("source") is not None:
                key = row["source"]
                if key in sources and sources[key] != row["remaining"]:
                    self.violation("conflicting_carcass_copies", sid)
                sources[key] = row["remaining"]
            old = self.previous_rows.get(sid)
            if old and "counters" in row and (old["guid"], old["life"]) == (row["guid"], row["life"]):
                delta = row["counters"]["harvested"] - old["counters"]["harvested"]
                key = row.get("source") or old.get("source")
                if delta > 0 and key is not None:
                    extraction[key] += delta
        for key, remaining in sources.items():
            before = self.previous_sources.get(key)
            if before is not None and (remaining > before or extraction[key] > before - remaining):
                self.violation("carcass_food_duplicated_or_not_debited", key[0])
        for sid in HUNTER_IDS:
            row = rows.get(sid)
            if row is None or not row["alive"]:
                self.interrupt(sid, "hunter_not_live")
                continue
            self.update_hunter(sid, row, prey)
        self.update_consumers(rows)
        self.previous_rows, self.previous_prey, self.previous_sources = rows, prey, sources

    def update_consumers(self, rows):
        pass

    def update_hunter(self, sid, row, prey):
        track = self.tracks[sid]
        old = track.previous
        if old and (old["guid"], old["life"], old["epoch"]) != (row["guid"], row["life"], row["epoch"]):
            self.interrupt(sid, "hunter_incarnation_changed")
            old = None
        track.samples += 1
        if old is None:
            track.anchor, track.since = row["position"], row["t"]
        displacement = distance(track.anchor, row["position"])
        track.maximum_displacement = max(track.maximum_displacement, displacement)
        track.continuous_seconds = max(track.continuous_seconds, row["t"] - track.since)
        if displacement >= 2:
            track.movements.setdefault(self.epoch, dict(start_seconds=track.since, end_seconds=row["t"], yards=round(displacement, 3)))
        if row["t"] - track.since >= self.minimum_seconds and displacement < 2 and self.epoch not in track.movements:
            self.violation("hunter_stationary_for_full_window", sid)
        delta = {key: 0 for key in COUNTERS}
        if old:
            dt = row["t"] - old["t"]
            speeds = [v for v in (old["movement"].get("run_speed"), row["movement"].get("run_speed")) if finite(v) and v > 0]
            if speeds and dist3(old["position"], row["position"]) > max(speeds) * dt + 3:
                self.violation("unphysical_position_jump", sid)
            delta = {key: row["counters"][key] - old["counters"][key] for key in COUNTERS}
            if any(value < 0 for value in delta.values()):
                self.interrupt(sid, "hunter_counters_reset")
                track.anchor, track.since = row["position"], row["t"]
                old = None
                delta = {key: 0 for key in COUNTERS}
            elif row["cargo"] - old["cargo"] != delta["harvested"] - delta["delivered"] - delta["lost"]:
                self.violation("cargo_changed_without_matching_transfer", sid)
            if old and (delta["delivered"] > 0 or delta["consumed"] > 0) and min(old["depot_distance"], row["depot_distance"]) > 3.01:
                self.violation("depot_transaction_away_from_depot", sid)
            if old and old["hunger"] - row["hunger"] >= 0.1 and delta["consumed"] == 0:
                self.violation("hunger_satisfied_without_food_debit", sid)
            if old and delta["harvested"] > 0 and old.get("source") is not None and old["remaining"] == 0:
                self.violation("harvest_without_observed_available_carcass", sid)
        if old:
            self.update_meal(sid, old, row, delta)
        self.update_cycle(sid, old, row, prey, delta)
        track.previous = row

    @staticmethod
    def activity(row, phase, activity):
        actor, role = row["agent"], row["role"]
        return (row["phase"] == phase and row["movement"].get("moving") is False and actor.get("in_combat") is False and
                role.get("phase") == "ACTING" and role.get("activity") == activity and
                actor.get("action") == "AMBIENT" and actor.get("action_source_goal") == "LOCAL_ACTIVITY")

    def update_meal(self, sid, old, row, delta):
        if (delta["consumed"] > 0 and self.activity(old, "EATING_DEPOT", "EAT") and
            old["depot_distance"] <= 3.01 and row["depot_distance"] <= 3.01 and
            positive_int(old["role"].get("started_at_ms")) and row["capture"] - old["role"]["started_at_ms"] >= 5000 and
            old["hunger"] - row["hunger"] >= 0.1 and row.get("depot_flow", {}).get("consumed", 0) >= delta["consumed"]):
            self.tracks[sid].meals.append(dict(epoch=self.epoch, hunter_spawn_id=sid, depot_id=row["depot"]["id"],
                seconds=row["t"], portions=delta["consumed"], hunger_before=old["hunger"], hunger_after=row["hunger"],
                stock_before=old["depot"]["stock"], stock_after=row["depot"]["stock"], distance_yards=round(row["depot_distance"], 3),
                aggregate_delivered_during_sample=row["depot_flow"]["delivered"],
                aggregate_consumed_during_sample=row["depot_flow"]["consumed"]))

    def update_cycle(self, sid, old, row, prey, delta):
        track = self.tracks[sid]
        actor, role = row["agent"], row["role"]
        target = (actor.get("target") or {}).get("spawn_id")
        if row["phase"] == "HUNTING" and role.get("phase") == "HUNTING":
            attempt = role.get("started_at_ms")
            if target not in PREY_IDS or not positive_int(attempt) or attempt > row["capture"]:
                self.archive_cycle(sid, "unresolved_hunt_target_or_attempt")
                return
            if track.cycle and (target != track.cycle["prey_spawn_id"] or attempt != track.cycle["hunt_started_at_ms"]):
                self.archive_cycle(sid, "new_hunt_attempt")
            victim = prey.get(target)
            if track.cycle is None and victim and victim["alive"]:
                track.cycle = dict(epoch=self.epoch, hunter_spawn_id=sid, runtime_guid=row["guid"], lifetime_ms=row["life"],
                    prey_spawn_id=target, prey_runtime_guid=victim["guid"], prey_lifetime_ms=victim["life"],
                    hunt_started_at_ms=attempt, depot_id=row["depot"]["id"], covered=["HUNTING"], times={"HUNTING": row["t"]})
        cycle = track.cycle
        if cycle is None:
            return
        victim, prior = prey.get(cycle["prey_spawn_id"]), self.previous_prey.get(cycle["prey_spawn_id"])
        if "HARVESTED" not in cycle["covered"]:
            if not victim or victim["guid"] != cycle["prey_runtime_guid"] or victim["alive"] and (
                victim["life"] != cycle["prey_lifetime_ms"] or "TARGET_DEATH" in cycle["covered"]):
                self.archive_cycle(sid, "prey_missing_respawned_or_changed")
                return
            source = row.get("source")
            if (prior and prior["alive"] and not victim["alive"] and prior["guid"] == victim["guid"] and
                row["phase"] in ("HUNTING", "HARVESTING") and (target == cycle["prey_spawn_id"] or source and source[0] == cycle["prey_spawn_id"])):
                if "TARGET_DEATH" not in cycle["covered"]:
                    cycle["covered"].append("TARGET_DEATH"); cycle["times"]["TARGET_DEATH"] = row["t"]
            harvesting = (row["phase"] == "HARVESTING" and role.get("phase") == "FEEDING" and
                actor.get("action") == "EAT" and actor.get("action_source_goal") == "FEED" and
                row["movement"].get("moving") is False and actor.get("in_combat") is False and
                not victim["alive"] and dist3(row["position"], victim["position"]) <= 5.01 and
                source is not None and source[0] == cycle["prey_spawn_id"] and row["remaining"] > 0 and
                "TARGET_DEATH" in cycle["covered"] and positive_int(role.get("started_at_ms")))
            if harvesting:
                if "source_life" in cycle and cycle["source_life"] != source[1]:
                    self.archive_cycle(sid, "carcass_incarnation_changed")
                    return
                cycle["source_life"] = source[1]
                cycle["harvesting_started_at_ms"] = role["started_at_ms"]
                if "HARVESTING" not in cycle["covered"]:
                    cycle["covered"].append("HARVESTING"); cycle["times"]["HARVESTING"] = row["t"]
            if old and delta["harvested"] > 0 and "HARVESTING" in cycle["covered"]:
                key = (cycle["prey_spawn_id"], cycle["source_life"])
                if (old.get("source") == row.get("source") == key and not victim["alive"] and
                    dist3(row["position"], victim["position"]) <= 5.01 and delta["harvested"] == row["cargo"] - old["cargo"] and
                    old["remaining"] - row["remaining"] >= delta["harvested"] and
                    row["capture"] - cycle["harvesting_started_at_ms"] >= 5000):
                    cycle["covered"].append("HARVESTED"); cycle["times"]["HARVESTED"] = row["t"]
                    cycle.update(harvested_portions=delta["harvested"], harvest_position=row["position"],
                        carcass_before=old["remaining"], carcass_after=row["remaining"], carry_travel_yards=0,
                        harvest_depot_distance=row["depot_distance"])
        if "HARVESTED" not in cycle["covered"]:
            return
        if row["cargo"] > 0:
            cycle["carry_travel_yards"] = max(cycle["carry_travel_yards"], distance(cycle["harvest_position"], row["position"]))
            if row["phase"] == "RETURNING_TO_DEPOT" and cycle["carry_travel_yards"] >= 2 and "RETURNING_TO_DEPOT" not in cycle["covered"]:
                cycle["covered"].append("RETURNING_TO_DEPOT"); cycle["times"]["RETURNING_TO_DEPOT"] = row["t"]
        if (self.activity(row, "DELIVERING", "WORK") and row["depot_distance"] <= 3.01 and
            "RETURNING_TO_DEPOT" in cycle["covered"] and positive_int(role.get("started_at_ms"))):
            cycle["delivery_started_at_ms"] = role["started_at_ms"]
            if "DELIVERING" not in cycle["covered"]:
                cycle["covered"].append("DELIVERING"); cycle["times"]["DELIVERING"] = row["t"]
        if (old and delta["delivered"] > 0 and "DELIVERING" in cycle["covered"] and
            old["cargo"] - row["cargo"] == delta["delivered"] and row["depot_distance"] <= 3.01 and
            row["capture"] - cycle["delivery_started_at_ms"] >= 5000 and
            row.get("depot_flow", {}).get("delivered", 0) >= delta["delivered"] and
            cycle["harvest_depot_distance"] > 3.01):
            cycle["covered"].append("DELIVERED"); cycle["times"]["DELIVERED"] = row["t"]
            cycle.update(delivered_portions=delta["delivered"], stock_before=old["depot"]["stock"],
                stock_after=row["depot"]["stock"], delivery_distance_yards=round(row["depot_distance"], 3),
                aggregate_delivered_during_sample=row["depot_flow"]["delivered"],
                aggregate_consumed_during_sample=row["depot_flow"]["consumed"])
            track.cycles.append(cycle)
            track.cycle = None

    def report(self):
        cycles = [cycle for track in self.tracks.values() for cycle in track.cycles]
        meals = [meal for track in self.tracks.values() for meal in track.meals]
        epochs = {cycle["epoch"] for cycle in cycles} | {meal["epoch"] for meal in meals} | {self.epoch}
        def coverage(epoch):
            return {cycle["hunter_spawn_id"] for cycle in cycles if cycle["epoch"] == epoch}
        chosen = max(epochs, key=lambda epoch: (len(coverage(epoch)), any(m["epoch"] == epoch for m in meals), epoch))
        missing = []
        if not self.population_frames:
            missing.append(f"one_fresh_complete_population_of_{len(self.owned_ids)}_owned_and_{100 - len(self.owned_ids)}_native")
        for sid in sorted(HUNTER_IDS):
            if chosen not in self.tracks[sid].movements:
                missing.append(f"physical_movement_at_least_2_yards_{sid}")
            if sid not in coverage(chosen):
                missing.append(f"continuous_hunt_harvest_carry_and_delivery_{sid}")
        if not any(meal["epoch"] == chosen for meal in meals):
            missing.append("one_physical_depot_meal_with_stock_debit_and_hunger_drop_in_same_epoch")
        violations = list(self.violations.values())
        status = "FAIL" if violations else "INCONCLUSIVE" if missing else "PASS"
        return dict(profile="supply-v8", status=status, exit_code=EXIT_CODES[status], samples=self.samples,
            fresh_samples=self.fresh, complete_population_frames=self.population_frames, expected_owned=len(self.owned_ids),
            expected_native=100 - len(self.owned_ids), selected_epoch=chosen, delivered_hunters=sorted(coverage(chosen)), cycles=cycles, meals=meals,
            agents={str(sid): dict(live_samples=t.samples, maximum_displacement_yards=round(t.maximum_displacement, 3),
                continuous_seconds=round(t.continuous_seconds, 3), movements=t.movements, cycles=len(t.cycles),
                partial_cycle=t.cycle or t.partial, interruptions=dict(t.interruptions)) for sid, t in self.tracks.items()},
            accounting=self.accounting_by_epoch.get(chosen), latest_accounting=self.latest_accounting,
            missing_evidence=missing, violations=violations, quality=dict(self.quality),
            minimum_stationary_observation_seconds=self.minimum_seconds,
            limitations=["Observed hunted-target death does not attribute the killing blow or exclude player intervention.",
                "Positive first-cycle proof does not establish long-term balance, restart persistence, or a completed stationary window.",
                "Carcass life is a runtime ledger incarnation counter, not the prey LivingRole lifetime timestamp."])


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
        report = evaluate_samples(recording_rows(args.path), minimum_seconds=args.minimum_seconds, max_gap_seconds=args.max_gap_seconds)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile="supply-v8", status="INCONCLUSIVE", exit_code=2,
                      missing_evidence=["readable_valid_recording"], input_error=type(error).__name__)
    rendered = json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
