"""Read-only physical farmer acceptance for fields-v8 recordings.

Each farmer needs one continuous observed field work, cargo, own-warehouse
delivery, meal and home-rest chain in a shared fresh ledger epoch. This gate
does not certify the older hunter/raid behavior gates or client animation pixels.
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
    from . import field_population as population, supply_acceptance as supply
except ImportError:
    import field_population as population
    import supply_acceptance as supply

EXIT_CODES = supply.EXIT_CODES
COUNTERS = (*supply.COUNTERS, "stolen")
ACCOUNTING = supply.ACCOUNTING
finite, point, dist3, counter, quantity = supply.finite, supply.point, supply.dist3, supply.counter, supply.quantity
recording_rows = supply.recording_rows
RADIUS = 3.01
START_LAG_MS = 1000
MAX_CHAINS_PER_WORKER = 4


@dataclass
class Track:
    previous: dict | None = None
    chain: dict | None = None
    attempts: dict = field(default_factory=dict)
    closed_stamps: set = field(default_factory=set)
    best_partial: dict | None = None
    chains: list = field(default_factory=list)
    censored: Counter = field(default_factory=Counter)


class _Evaluator:
    def __init__(self, minimum_seconds=300, max_gap_seconds=2):
        if not finite(minimum_seconds) or minimum_seconds <= 0 or not finite(max_gap_seconds) or not 0 < max_gap_seconds <= 5:
            raise ValueError("Observation duration must be positive and capture gap must be in (0,5] seconds")
        self.population = population.Population.load()
        self.actors = {actor.spawn_id: actor for actor in self.population.settlement.actors}
        self.managed = {actor.spawn_id: actor for actor in self.population.actors}
        self.workers = {actor.spawn_id: actor for actor in self.population.recruits}
        self.fields = {definition["id"]: definition for definition in self.population.fields}
        self.depots = {depot["id"]: depot["position"] for depot in population.raids.consumers.supply.DEPOTS}
        self.depots[3] = population.raids.DEPOT["position"]
        self.depots.update({key: definition["warehouse"]["position"] for key, definition in self.fields.items()})
        self.members = {1: {901034, 901035, 901036, 901039}, 2: {901061, 901062, 901063}, 3: {901055},
                        **{key: set(workers) for key, workers in population.FIELD_WORKERS.items()}}
        self.tracks = {sid: Track() for sid in sorted(self.workers)}
        self.minimum_seconds, self.max_gap = minimum_seconds, max_gap_seconds
        self.quality, self.violations, self.epochs = Counter(), {}, {}
        self.samples = self.fresh = self.complete = self.direct_sequence = self.epoch = 0
        self.session = self.previous_session = self.last_sequence = self.last_time = None
        self.last_capture = self.last_payload = self.last_unique_time = None
        self.previous_metrics = self.previous_depots = self.previous_crops = self.previous_accounting = None

    def violation(self, code, sid=0):
        self.violations[(code, sid)] = dict(code=code, spawn_id=sid)

    def censor_track(self, sid, reason):
        track = self.tracks[sid]
        if track.chain:
            partial = {**track.chain, "censored_by": reason}
            if track.best_partial is None or len(partial["covered"]) > len(track.best_partial["covered"]):
                track.best_partial = partial
        track.censored[reason] += 1
        track.previous = track.chain = None
        track.attempts.clear()
        track.closed_stamps.clear()

    def reset(self, reason):
        self.epoch += 1
        self.quality[reason] += 1
        for sid in self.tracks: self.censor_track(sid, reason)
        self.previous_metrics = self.previous_depots = self.previous_crops = self.previous_accounting = None

    def usable(self, record):
        if not isinstance(record, dict):
            self.reset("invalid_record"); return None
        if record.get("kind") == "session":
            self.session = record.get("session_id"); return None
        if record.get("kind") == "summary": return None
        direct = "state" not in record and "agents" in record
        if direct:
            self.direct_sequence += 1
            capture = record.get("captured_at_ms")
            record = dict(state=record, status="fresh", sequence=self.direct_sequence,
                          elapsed_seconds=capture / 1000 if finite(capture) else None)
        self.samples += 1
        state, t, sequence = record.get("state"), record.get("elapsed_seconds"), record.get("sequence")
        if not isinstance(state, dict) or not finite(t) or t < 0 or not supply.positive_int(sequence):
            self.reset("invalid_sample"); return None
        session, capture = record.get("session_id", self.session), state.get("captured_at_ms")
        same = self.last_capture is not None and capture == self.last_capture
        continuous = self.last_time is None or (session == self.previous_session and sequence == self.last_sequence + 1 and
            (0 < t - self.last_time <= self.max_gap or direct and same and t == self.last_time))
        if not continuous: self.reset("session_sequence_or_time_gap")
        self.previous_session, self.last_time, self.last_sequence = session, t, sequence
        if (record.get("status") != "fresh" or state.get("configured") is not True or state.get("stale") is not False or
            state.get("version") != 4 or not supply.positive_int(capture) or type(state.get("age_ms")) is not int or
            not 0 <= state["age_ms"] < 5000 or not isinstance(state.get("agents"), list)):
            self.reset("stale_or_unusable_snapshot"); return None
        try:
            payload = json.dumps({key: state.get(key) for key in ("version", "scope", "agents")},
                                 sort_keys=True, separators=(",", ":"), allow_nan=False)
        except (ValueError, TypeError):
            self.reset("invalid_payload"); return None
        if same:
            if continuous and payload == self.last_payload and self.last_unique_time is not None and 0 <= t-self.last_unique_time <= self.max_gap:
                self.quality["identical_capture_ignored"] += 1; return None
            self.reset("mutated_repeat_or_duplicate_gap"); return None
        if self.last_capture is not None and (not 0 < capture-self.last_capture <= self.max_gap*1000 or
            self.last_unique_time is not None and not 0 < t-self.last_unique_time <= self.max_gap):
            self.reset("capture_reset_or_gap")
        self.last_capture, self.last_payload, self.last_unique_time = capture, payload, t
        return state, t, capture

    def validate_population(self, agents):
        if len(agents) > 100 or any(not isinstance(actor, dict) for actor in agents):
            self.violation("foreign_or_invalid_population"); self.reset("invalid_population"); return None
        ids = [actor.get("spawn_id") for actor in agents]
        if any(not supply.positive_int(sid) for sid in ids) or len(set(ids)) != len(ids):
            self.reset("invalid_or_duplicate_population"); return None
        if set(ids) - self.actors.keys(): self.violation("foreign_spawn_in_population")
        if set(ids) != self.actors.keys():
            self.reset("incomplete_100_actor_population"); return None
        by_id = {actor["spawn_id"]: actor for actor in agents}
        for sid, actor in by_id.items():
            expected, owned = self.actors[sid], sid in population.SPAWN_IDS
            if (actor.get("agent_id") != sid or actor.get("entry") != expected.entry or
                actor.get("native_creature") is not (not owned) or
                actor.get("control_mode") != ("AI_WORLD_CONTROLLED" if owned else "OBSERVE_ONLY")):
                self.violation("identity_entry_or_ownership_changed", sid)
            if owned:
                binding = self.managed[sid]
                if (actor.get("type") != {0: "CIVILIAN", 1: "GUARD", 5: "COMBATANT", 6: "PREDATOR", 7: "PREY"}[binding.agent_type] or
                    type(actor.get("world_faction")) is not int or actor["world_faction"] != getattr(binding, "world_faction", 0)):
                    self.violation("ordinary_managed_type_or_faction_changed", sid)
            home = point(actor.get("home"))
            if home is None:
                self.reset("missing_immutable_home"); return None
            if (actor["home"].get("map_id") != 725 or dist3(home, (expected.x, expected.y, expected.z)) > .01 or actor.get("work") is not None):
                self.violation("immutable_home_or_database_work_changed", sid)
            pos = actor.get("position")
            if not isinstance(pos, dict) or pos.get("map_id") != 725:
                self.violation("actor_left_authored_map", sid)
            if not owned and any(actor.get(key) is not None for key in ("needs", "living_role", "supply", "raid", "economy")):
                self.violation("native_actor_acquired_ai_state", sid)
        return by_id

    def worker_row(self, actor, t, capture):
        sid, spec = actor["spawn_id"], self.workers[actor["spawn_id"]]
        pos, role, supplied = actor.get("position"), actor.get("living_role"), actor.get("supply")
        movement, needs, economy = actor.get("movement"), actor.get("needs"), actor.get("economy")
        if (not all(isinstance(value, dict) for value in (pos, role, supplied, movement, needs, economy)) or
            pos.get("source") != "live" or point(pos) is None or actor.get("world_state") != "MATERIALIZED" or
            actor.get("alive") is not True or not finite(actor.get("health")) or actor["health"] <= 0 or
            not isinstance(actor.get("runtime_guid"), str) or not actor["runtime_guid"].strip() or
            not supply.positive_int(role.get("lifetime_ms")) or role["lifetime_ms"] > capture):
            return None
        if (actor.get("type") != "CIVILIAN" or type(actor.get("world_faction")) is not int or actor["world_faction"] != 1 or
            role.get("role") != "FARMER" or role.get("enabled") is not True or role.get("extensions_enabled") is not True or
            supplied.get("enabled") is not True or supplied.get("role") != "FARMER" or
            type(supplied.get("field_id")) is not int or supplied["field_id"] != spec.field_id or
            supplied.get("depot_id") != spec.depot_id or supplied.get("capacity") != 2 or not quantity(supplied.get("cargo"), 2) or
            not isinstance(supplied.get("depot"), dict) or supplied["depot"].get("id") != spec.depot_id or
            any(supplied.get(key) is not None for key in ("hunting_ground", "carcass_remaining", "carcass_spawn_id", "carcass_life"))):
            self.violation("farmer_role_field_or_capability_changed", sid); return None
        index, definition = supplied.get("field_workpoint_index"), self.fields[spec.field_id]
        if (type(index) is not int or index not in tuple(spec.worker_ordinal+4*cycle for cycle in range(3)) or
            point(supplied.get("field_workpoint")) is None or supplied["field_workpoint"].get("map_id") != 725 or
            dist3(point(supplied["field_workpoint"]), point(definition["work_points"][index])) > .01 or
            not quantity(supplied.get("field_remaining"), 24) or supplied.get("field_capacity") != 24):
            self.violation("farmer_workpoint_rotation_or_crop_bounds_changed", sid); return None
        values = {key: counter(supplied.get(key, "0" if key == "stolen" else None)) for key in COUNTERS}
        if any(value is None for value in values.values()) or values["stolen"]:
            self.violation("invalid_farmer_food_counters", sid); return None
        if not finite(needs.get("hunger")) or not 0 <= needs["hunger"] <= 1 or not quantity(economy.get("food")):
            return None
        xyz, home = point(pos), point(actor["home"])
        if not finite(movement.get("home_distance")) or abs(movement["home_distance"]-dist3(xyz, home)) > 1:
            self.violation("runtime_home_distance_changed", sid); return None
        return dict(agent=actor, supply=supplied, role=role, movement=movement, position=xyz, home=home,
            guid=actor["runtime_guid"], life=role["lifetime_ms"], capture=capture, t=t, epoch=self.epoch,
            hunger=needs["hunger"], personal_food=economy["food"], index=index, counters=values, cargo=supplied["cargo"],
            field_distance=dist3(xyz, point(definition["work_points"][index])),
            depot_distance=dist3(xyz, point(definition["warehouse"]["position"])), home_distance=dist3(xyz, home))

    def ledger(self, actors):
        depots, metrics, accounting, crops = {}, {}, None, {}
        for sid, actor in actors.items():
            supplied, raid = actor.get("supply"), actor.get("raid")
            copies = []
            if isinstance(supplied, dict):
                raw = supplied.get("accounting")
                if not isinstance(raw, dict) or any(counter(raw.get(key)) is None for key in ACCOUNTING): return None
                current = {key: counter(raw[key]) for key in ACCOUNTING}
                if raw.get("conserved") is not True or current["initial_food"]+current["harvested"] != sum(current[key] for key in ("in_depots", "in_cargo", "consumed", "lost")):
                    self.violation("food_conservation_broken")
                if accounting is not None and accounting != current: self.violation("conflicting_accounting_copies")
                accounting = current
                role = actor.get("living_role")
                if not isinstance(role, dict): return None
                values = {key: counter(supplied.get(key, "0" if key == "stolen" else None)) for key in COUNTERS}
                if any(value is None for value in values.values()) or not quantity(supplied.get("cargo")):
                    self.violation("invalid_supply_counter_or_cargo", sid); return None
                metrics[sid] = dict(counters=values, cargo=supplied["cargo"], guid=actor.get("runtime_guid"),
                    life=role.get("lifetime_ms"))
                if isinstance(supplied.get("depot"), dict): copies.append(supplied["depot"])
                if sid in self.workers:
                    fid = self.workers[sid].field_id
                    crop = (supplied.get("field_remaining"), supplied.get("field_capacity"))
                    if fid in crops and crops[fid] != crop: self.violation("conflicting_crop_field_copies", sid)
                    crops[fid] = crop
            if isinstance(raid, dict): copies.extend((raid.get("source_depot"), raid.get("camp_depot")))
            for depot in copies:
                if not isinstance(depot, dict): return None
                key = depot.get("id")
                if type(key) is not int or key not in self.depots:
                    self.violation("foreign_food_depot", sid); return None
                if (point(depot.get("position")) is None or depot["position"].get("map_id") != 725 or
                    dist3(point(depot["position"]), point(self.depots[key])) > .01 or
                    depot.get("capacity") != 30 or depot.get("low_threshold") != 4 or not quantity(depot.get("stock"), 30) or
                    any(counter(depot.get(name, "0" if name == "stolen" else None)) is None for name in ("delivered", "consumed", "stolen"))):
                    self.violation("food_depot_position_capacity_or_quantity_changed", key); return None
                if key in depots and depots[key] != depot: self.violation("conflicting_depot_copies", key)
                depots[key] = depot
        if accounting is None or set(depots) != set(self.depots) or set(crops) != set(self.fields) or not set.union(*self.members.values()).issubset(metrics):
            return None
        if sum(depot["stock"] for depot in depots.values()) != accounting["in_depots"]:
            self.violation("depot_total_disagrees_with_accounting")
        if sum(value["cargo"] for value in metrics.values()) != accounting["in_cargo"]:
            self.violation("cargo_total_disagrees_with_accounting")
        return metrics, depots, crops, accounting

    @staticmethod
    def activity(row, kind):
        phase, activity = {"work": ("WORKING_FIELD", "WORK"), "delivery": ("DELIVERING", "WORK"),
                           "meal": ("EATING_DEPOT", "EAT"), "rest": ("RESTING_HOME", "REST")}[kind]
        actor, role = row["agent"], row["role"]
        return (row["supply"].get("phase") == phase and role.get("phase") == "ACTING" and role.get("activity") == activity and
            actor.get("action") == "AMBIENT" and actor.get("action_source_goal") == "LOCAL_ACTIVITY" and
            actor.get("goal") == "LOCAL_ACTIVITY" and actor.get("goal_owner") == "INDIVIDUAL" and
            supply.positive_int(role.get("started_at_ms")) and actor.get("action_started_at_ms") == role["started_at_ms"] and
            role["started_at_ms"] <= row["capture"] and actor.get("in_combat") is False and row["movement"].get("moving") is False)

    @staticmethod
    def moving(row, purpose):
        actor, role = row["agent"], row["role"]
        return (role.get("phase") == "MOVING" and role.get("movement_purpose") == purpose and
            actor.get("action") == "MOVE_TO" and actor.get("action_source_goal") == "LOCAL_ACTIVITY" and
            actor.get("goal") == "LOCAL_ACTIVITY" and actor.get("goal_owner") == "INDIVIDUAL" and
            supply.positive_int(role.get("started_at_ms")) and role["started_at_ms"] <= row["capture"] and
            actor.get("action_started_at_ms") == role["started_at_ms"] and
            row["movement"].get("moving") is True and actor.get("in_combat") is False)

    def owned_event(self, sid, kind, old, row, duration):
        track = self.tracks[sid]
        anchor = "field_distance" if kind == "work" else "depot_distance"
        if (not self.activity(old, kind) or old[anchor] > RADIUS or row[anchor] > RADIUS or
            row["agent"].get("in_combat") is not False or row["capture"]-old["role"].get("started_at_ms", row["capture"]) < duration):
            self.violation(kind + "_without_owned_action_position_or_duration", sid); return False
        attempt = track.attempts.get(kind)
        if (not attempt or attempt["started"] != old["role"]["started_at_ms"] or not attempt["start_observed"] or
            old["capture"] != attempt["last_capture"] or row["capture"]-attempt["last_capture"] > self.max_gap*1000):
            track.censored[kind + "_start_or_continuity_not_observed"] += 1; return False
        return True

    def update_worker(self, sid, row, flows):
        track, spec = self.tracks[sid], self.workers[sid]
        old = track.previous
        if old:
            elapsed, step = (row["capture"]-old["capture"])/1000, dist3(old["position"], row["position"])
            speeds = [value for value in (old["movement"].get("run_speed"), row["movement"].get("run_speed")) if finite(value) and value > 0]
            if not speeds:
                self.censor_track(sid, "missing_movement_speed"); old = None
            elif step > max(speeds)*elapsed+3: self.violation("unphysical_position_jump", sid)
        delta = {key: row["counters"][key]-old["counters"][key] if old else 0 for key in COUNTERS}
        if old:
            if row["cargo"]-old["cargo"] != delta["harvested"]-delta["delivered"]-delta["lost"]:
                self.violation("farmer_cargo_changed_without_transfer", sid)
            if old["hunger"]-row["hunger"] > .000001 and not delta["consumed"]:
                self.violation("farmer_hunger_relief_without_depot_debit", sid)
            if row["personal_food"] > old["personal_food"]: self.violation("farmer_created_personal_economy_food", sid)
            if row["index"] != old["index"]:
                expected = spec.worker_ordinal + 4 * (((old["index"]-spec.worker_ordinal)//4+1) % 3)
                if row["index"] != expected or not self.activity(old, "rest") or old["home_distance"] > RADIUS or row["capture"]-old["role"].get("started_at_ms", row["capture"]) < 10000:
                    self.violation("workpoint_rotation_without_owned_home_rest", sid)
            chain = track.chain
            if chain and (self.moving(old, "FARM_FIELD") or self.moving(row, "FARM_FIELD")): chain["field_travel_yards"] += step
            if chain and (self.moving(old, "FARM_DEPOT") or self.moving(row, "FARM_DEPOT")): chain["warehouse_travel_yards"] += step
            if chain and (self.moving(old, "FARM_HOME") or self.moving(row, "FARM_HOME")): chain["home_travel_yards"] += step
            if delta["harvested"]:
                valid = self.owned_event(sid, "work", old, row, 15000)
                if delta["harvested"] != 1 or old["supply"]["field_remaining"] <= 0:
                    self.violation("field_harvest_not_one_available_portion", sid); valid = False
                if valid and chain and "FIELD_ARRIVED" in chain["covered"]:
                    chain["covered"] = list(dict.fromkeys(chain["covered"]+["HARVESTED", "CARGO"]))
                    chain["harvested_seconds"] = row["t"]
            if delta["delivered"]:
                valid = self.owned_event(sid, "delivery", old, row, 5000)
                if not 0 < delta["delivered"] <= old["cargo"] <= 2 or flows[spec.depot_id]["delivered"] < delta["delivered"]:
                    self.violation("delivery_without_cargo_or_reconciled_depot_credit", sid); valid = False
                if valid and chain and "CARGO" in chain["covered"] and chain["warehouse_travel_yards"] >= 2:
                    chain["covered"] = list(dict.fromkeys(chain["covered"]+["DELIVERED"]))
                    chain["delivered_seconds"] = row["t"]
            if delta["consumed"]:
                valid = self.owned_event(sid, "meal", old, row, 5000)
                if (delta["consumed"] != 1 or old["hunger"]-row["hunger"] < .1 or
                    flows[spec.depot_id]["consumed"] < 1 or counter(row["supply"]["accounting"]["consumed"])-counter(old["supply"]["accounting"]["consumed"]) < 1):
                    self.violation("meal_without_hunger_relief_and_reconciled_debit", sid); valid = False
                if valid and chain and "DELIVERED" in chain["covered"]:
                    chain["covered"] = list(dict.fromkeys(chain["covered"]+["MEAL"]))
                    chain["meal_seconds"] = row["t"]
        if self.moving(row, "FARM_FIELD") and row["field_distance"] > RADIUS and track.chain is None:
            track.chain = dict(epoch=self.epoch, spawn_id=sid, runtime_guid=row["guid"], lifetime_ms=row["life"],
                field_id=spec.field_id, depot_id=spec.depot_id, workpoint_index=row["index"],
                departure_seconds=row["t"], covered=["TO_FIELD"], field_travel_yards=0, warehouse_travel_yards=0, home_travel_yards=0)
        chain = track.chain
        for kind in ("work", "delivery", "meal", "rest"):
            valid = self.activity(row, kind)
            distance = row["field_distance"] if kind == "work" else row["home_distance"] if kind == "rest" else row["depot_distance"]
            attempt = track.attempts.get(kind)
            started = row["role"].get("started_at_ms")
            if valid and distance <= RADIUS:
                if attempt is None or attempt["started"] != started:
                    if attempt: track.closed_stamps.add((kind, attempt["started"]))
                    track.attempts[kind] = dict(started=started, first_capture=row["capture"], last_capture=row["capture"],
                        start_observed=0 <= row["capture"]-started <= START_LAG_MS and (kind, started) not in track.closed_stamps)
                else: attempt["last_capture"] = row["capture"]
                if kind == "work" and chain and chain["field_travel_yards"] >= 2:
                    chain["covered"] = list(dict.fromkeys(chain["covered"]+["FIELD_ARRIVED"]))
                if kind == "rest" and chain and "DELIVERED" in chain["covered"]:
                    if "MEAL" in chain["covered"] and (chain["home_travel_yards"] >= 2 or dist3(row["home"], point(self.depots[spec.depot_id])) <= RADIUS):
                        chain["covered"] += ["HOME_REST"]
                        chain["returned_seconds"], chain["home_distance_yards"] = row["t"], row["home_distance"]
                        track.chains.append(chain)
                        del track.chains[:-MAX_CHAINS_PER_WORKER]
                        track.chain = None
                    elif "MEAL" not in chain["covered"]:
                        self.censor_track(sid, "returned_home_before_a_depot_meal")
            elif attempt:
                track.closed_stamps.add((kind, attempt["started"]))
                del track.attempts[kind]
            if len(track.closed_stamps) > 64:
                # Only recent action stamps can be reused in an ordered stream.
                track.closed_stamps = set(sorted(track.closed_stamps, key=lambda item: item[1])[-64:])
        track.previous = row

    def observe(self, record):
        usable = self.usable(record)
        if usable is None: return
        state, t, capture = usable
        actors = self.validate_population(state["agents"])
        if actors is None: return
        self.fresh += 1
        rows = {sid: self.worker_row(actors[sid], t, capture) for sid in self.workers}
        if any(row is None for row in rows.values()):
            self.reset("farmer_not_live_or_incomplete"); return
        ledger = self.ledger(actors)
        if ledger is None:
            self.reset("incomplete_six_depot_ledger"); return
        metrics, depots, crops, accounting = ledger
        if self.previous_metrics:
            comparable = set(metrics) == set(self.previous_metrics) and all(
                (metrics[sid]["guid"], metrics[sid]["life"]) == (self.previous_metrics[sid]["guid"], self.previous_metrics[sid]["life"])
                for sid in metrics)
            reset = (not comparable or any(metrics[sid]["counters"][key] < self.previous_metrics[sid]["counters"][key] for sid in metrics for key in COUNTERS) or
                accounting["initial_food"] != self.previous_accounting["initial_food"] or
                any(accounting[key] < self.previous_accounting[key] for key in ("harvested", "consumed", "lost")))
            if reset: self.reset("incarnation_or_food_counter_transition")
        flows = {key: dict(delivered=0, consumed=0, stolen=0) for key in self.depots}
        if self.previous_metrics:
            changes = {sid: {key: metrics[sid]["counters"][key]-self.previous_metrics[sid]["counters"][key] for key in COUNTERS} for sid in metrics}
            for key, members in self.members.items():
                flow = flows[key]
                for name in ("delivered", "consumed"): flow[name] = sum(changes[sid][name] for sid in members)
                flow["stolen"] = changes[901055]["stolen"] if key == 1 else 0
                before, after = self.previous_depots[key], depots[key]
                if (after["stock"]-before["stock"] != flow["delivered"]-flow["consumed"]-flow["stolen"] or
                    any(counter(after.get(name, "0"))-counter(before.get(name, "0")) != flow[name] for name in flow)):
                    self.violation("depot_stock_change_not_reconciled", key)
            for key in ("harvested", "consumed", "lost"):
                if accounting[key]-self.previous_accounting[key] != sum(value[key] for value in changes.values()):
                    self.violation("global_food_counter_change_not_reconciled")
            for key, members in population.FIELD_WORKERS.items():
                regrown = crops[key][0]-self.previous_crops[key][0]+sum(changes[sid]["harvested"] for sid in members)
                if not 0 <= regrown <= 1: self.violation("field_potential_not_debited_or_unbounded_regrowth", key)
        for row in rows.values(): row["epoch"] = self.epoch
        self.complete += 1
        epoch = self.epochs.setdefault(self.epoch, dict(first_capture=capture, last_capture=capture, frames=0))
        epoch["last_capture"], epoch["frames"] = capture, epoch["frames"]+1
        for sid, row in rows.items(): self.update_worker(sid, row, flows)
        self.previous_metrics, self.previous_depots, self.previous_crops, self.previous_accounting = metrics, depots, crops, accounting

    def report(self):
        chains = [chain for track in self.tracks.values() for chain in track.chains]
        selected = max(self.epochs, key=lambda epoch: (len({chain["spawn_id"] for chain in chains if chain["epoch"] == epoch}),
            self.epochs[epoch]["last_capture"]-self.epochs[epoch]["first_capture"]), default=None)
        completed = {chain["spawn_id"] for chain in chains if chain["epoch"] == selected}
        seconds = (self.epochs[selected]["last_capture"]-self.epochs[selected]["first_capture"])/1000 if selected is not None else 0
        missing = [f"field_cargo_delivery_meal_home_chain_{sid}" for sid in sorted(self.workers.keys()-completed)]
        if seconds < self.minimum_seconds: missing.append("shared_continuous_fresh_observation_window")
        violations = list(self.violations.values())
        status = "FAIL" if violations else "INCONCLUSIVE" if missing else "PASS"
        return dict(profile=population.PROFILE, status=status, exit_code=EXIT_CODES[status], samples=self.samples,
            fresh_samples=self.fresh, complete_population_frames=self.complete, expected_owned=40, expected_native=60,
            expected_farmers=12, completed_farmers=sorted(completed), selected_epoch=selected,
            observed_seconds=seconds, minimum_observation_seconds=self.minimum_seconds, max_gap_seconds=self.max_gap,
            maximum_action_start_capture_lag_ms=START_LAG_MS, chains=chains,
            workers={str(sid): dict(chain_count=len(track.chains), partial_chain=track.chain or track.best_partial,
                censored_evidence=dict(track.censored)) for sid, track in self.tracks.items()},
            quality=dict(self.quality), violations=violations, missing_evidence=missing,
            limitations=["Action/position snapshots and source-validated work-emote dispatch do not prove client animation pixels or independently measure collision and line of sight.",
                "A completion capture must follow the exact owned start by 15/5/5 seconds; the first capture may lag by at most one second and each intervening gap is bounded by max_gap_seconds. The completion falls between the last active and completion captures.",
                "Home and warehouse intentionally coincide for each village's first worker, so owned home rest there does not require a fictitious return walk.",
                "Crop regrowth and harvest can share a producer tick; remaining potential is checked with at most one regrown portion per bounded capture interval.",
                "This field gate does not replace the separate hunter, consumer or raid physical acceptance gates."])


def evaluate_samples(samples: Iterable[dict], *, minimum_seconds=300, max_gap_seconds=2) -> dict:
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds)
    for sample in samples: evaluator.observe(sample)
    return evaluator.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="closed recorder directory, .jsonl or .jsonl.gz")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=300)
    parser.add_argument("--max-gap-seconds", type=float, default=2)
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(recording_rows(args.path), minimum_seconds=args.minimum_seconds, max_gap_seconds=args.max_gap_seconds)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile=population.PROFILE, status="INCONCLUSIVE", exit_code=2,
                      missing_evidence=["readable_valid_recording"], input_error=type(error).__name__)
    rendered = json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2)+"\n"
    if args.output: args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__": raise SystemExit(main())
