"""Read-only physical food-caravan acceptance for full-v8 recordings.

The older six-depot field/raid gates do not describe a seven-depot caravan
ledger. This gate audits actual available copies and never fills missing dead
or unloaded actors with inferred counters. PASS requires a linked, original-
incarnation escorted delivery from each village, not just rising city stock.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path

try:
    from . import field_population as fields, full_population as full, supply_acceptance as common
except ImportError:
    import field_population as fields
    import full_population as full
    import supply_acceptance as common

EXIT_CODES = common.EXIT_CODES
finite, point, distance, counter, quantity = common.finite, common.point, common.dist3, common.counter, common.quantity
SOURCES = frozenset((1, 4, 5, 6))
RADIUS = 1.92
METRICS = ("harvested", "delivered", "consumed", "lost", "stolen", "loaded")
DEPOT_COUNTERS = ("delivered", "consumed", "stolen", "exported")
FOOD_ACTORS = frozenset((901034, 901035, 901036, 901039, 901061, 901062, 901063,
    901076, 901081, 901086, 901091, *range(901044, 901060), *range(901016, 901020),
    *range(901022, 901026), *range(901028, 901032), *full.SUPPLY_RECRUIT_IDS))
PAIRS = {pair["id"]: pair for pair in full.CARAVAN_PAIRS}


def depot_specs():
    result = {value["id"]: value for value in fields.raids.consumers.supply.DEPOTS}
    result[3] = fields.raids.DEPOT
    result.update({definition["id"]: definition["warehouse"] for definition in fields.Population.load().fields})
    result[7] = full.CITY_DEPOT
    return result


def valid_process(proof):
    if not isinstance(proof, dict) or proof.get("process_unchanged") is not True:
        return None
    identity = proof.get("actual_running_binary")
    if not isinstance(identity, dict):
        return None
    fingerprint = tuple(identity.get(key) for key in ("pid", "start_ticks", "device", "inode", "sha256", "version"))
    if (any(value is None for value in fingerprint) or type(fingerprint[0]) is not int or fingerprint[0] <= 0 or
        not str(fingerprint[1]).isascii() or not str(fingerprint[1]).isdecimal() or int(fingerprint[1]) <= 0 or
        any(type(value) is not int or value <= 0 for value in fingerprint[2:4]) or
        not isinstance(fingerprint[4], str) or len(fingerprint[4]) != 64 or
        any(char not in "0123456789abcdef" for char in fingerprint[4]) or
        not isinstance(fingerprint[5], str) or not fingerprint[5]):
        return None
    return fingerprint


@dataclass
class Trip:
    identity: tuple
    source: int
    bindings: tuple
    assembled_at: int
    outward: dict = field(default_factory=dict)
    returning: dict = field(default_factory=dict)
    joint_outward: int = 0
    joint_return: int = 0
    outward_targets: dict = field(default_factory=dict)
    return_targets: dict = field(default_factory=dict)
    load: dict | None = None
    delivered: int = 0
    closed: bool = False


class _Evaluator:
    def __init__(self, minimum_seconds=300, max_gap_seconds=2, *, process_proof=None):
        if not finite(minimum_seconds) or minimum_seconds <= 0 or not finite(max_gap_seconds) or not 0 < max_gap_seconds <= 5:
            raise ValueError("Duration must be positive and the maximum gap must be in (0,5] seconds")
        self.minimum_seconds, self.max_gap = minimum_seconds, max_gap_seconds
        self.specs = depot_specs()
        self.process_proof, self.process = process_proof, valid_process(process_proof)
        self.quality, self.violations = Counter(), {}
        self.audited_epochs = Counter()
        self.epoch = self.samples = self.unique = self.complete = self.audited_intervals = 0
        self.last_capture = self.last_payload = self.last_sequence = self.last_elapsed = None
        self.session = self.last_session = None
        self.previous_ledger = None
        self.previous_pairs, self.trips, self.activities, self.spent = {}, {}, {}, set()
        self.epochs, self.receipts = {}, []
        if isinstance(process_proof, dict) and process_proof.get("process_unchanged") is False:
            self.violation("actual_world_process_changed")

    def violation(self, code, identity=0):
        self.violations[(code, identity)] = dict(code=code, identity=identity)

    def censor(self, reason):
        self.quality[reason] += 1
        self.epoch += 1
        self.previous_ledger = None
        self.previous_pairs.clear()
        self.trips.clear()
        self.activities.clear()

    def usable(self, record):
        if not isinstance(record, dict):
            self.censor("invalid_record"); return None
        if record.get("kind") == "session":
            self.session = record.get("session_id"); return None
        if record.get("kind") == "summary": return None
        self.samples += 1
        direct = "agents" in record and "state" not in record
        state = record if direct else record.get("state")
        capture = state.get("captured_at_ms") if isinstance(state, dict) else None
        if (not isinstance(state, dict) or state.get("version") != 4 or state.get("configured") is not True or
            state.get("stale") is not False or not common.positive_int(capture) or
            type(state.get("age_ms")) is not int or not 0 <= state["age_ms"] < 5000 or
            not isinstance(state.get("agents"), list) or (not direct and record.get("status") != "fresh")):
            self.censor("stale_or_unusable_snapshot"); return None
        session = record.get("session_id", self.session)
        sequence = None if direct else record.get("sequence")
        elapsed = capture / 1000 if direct else record.get("elapsed_seconds")
        if not finite(elapsed) or elapsed < 0 or (not direct and not common.positive_int(sequence)):
            self.censor("invalid_sample_clock"); return None
        try:
            payload = json.dumps({key: state.get(key) for key in ("version", "scope", "agents")},
                                 sort_keys=True, separators=(",", ":"), allow_nan=False)
        except (TypeError, ValueError):
            self.censor("invalid_payload"); return None
        observed_process = record.get("process_identity", record.get("actual_running_binary"))
        if observed_process is not None:
            observed = valid_process(dict(process_unchanged=True, actual_running_binary=observed_process))
            if observed is None or self.process is not None and observed != self.process:
                self.violation("actual_world_process_changed")
        if self.last_capture == capture:
            if payload != self.last_payload:
                self.violation("mutated_duplicate_capture")
            else:
                self.quality["identical_capture_ignored"] += 1
                if session != self.last_session or not 0 <= elapsed-self.last_elapsed <= self.max_gap:
                    self.censor("duplicate_session_or_elapsed_gap")
                if not direct: self.last_sequence = sequence
            return None
        if self.last_capture is not None and (not 0 < capture-self.last_capture <= self.max_gap*1000 or
            not 0 < elapsed-self.last_elapsed <= self.max_gap or session != self.last_session or
            (not direct and self.last_sequence is not None and sequence != self.last_sequence+1)):
            self.censor("session_sequence_or_capture_gap")
        self.last_capture, self.last_payload, self.last_sequence = capture, payload, sequence
        self.last_elapsed, self.last_session = elapsed, session
        self.unique += 1
        window = self.epochs.setdefault(self.epoch, dict(first_capture_ms=capture, last_capture_ms=capture, frames=0))
        window["last_capture_ms"], window["frames"] = capture, window["frames"]+1
        return state, capture

    def ledger(self, actors):
        depots, metrics, accounting = {}, {}, None
        for sid, actor in actors.items():
            supplied, raid = actor.get("supply"), actor.get("raid")
            copies = []
            if isinstance(supplied, dict):
                if sid not in FOOD_ACTORS:
                    self.violation("unassigned_food_participant", sid)
                values = {key: counter(supplied.get(key, "0" if key in ("loaded", "stolen") else None)) for key in METRICS}
                caravan = supplied.get("caravan")
                values["loaded"] = counter(caravan.get("loaded")) if isinstance(caravan, dict) else 0
                if any(value is None for value in values.values()) or not quantity(supplied.get("cargo")):
                    self.violation("invalid_actor_food_metrics", sid)
                else:
                    metrics[sid] = dict(values, cargo=supplied["cargo"], depot_id=supplied.get("depot_id"),
                        source=(raid or {}).get("source_depot_id"), caravan_source=(caravan or {}).get("source_depot_id"))
                raw = supplied.get("accounting")
                if isinstance(raw, dict) and all(counter(raw.get(key)) is not None for key in common.ACCOUNTING):
                    current = {key: counter(raw[key]) for key in common.ACCOUNTING}
                    if (raw.get("conserved") is not True or current["initial_food"] != 0 or
                        current["harvested"] != sum(current[k] for k in ("in_depots", "in_cargo", "consumed", "lost"))):
                        self.violation("food_conservation_broken")
                    if accounting is not None and current != accounting:
                        self.violation("conflicting_food_accounting_copies")
                    accounting = current
                else:
                    self.quality["missing_accounting_copy"] += 1
                if supplied.get("depot") is not None: copies.append(supplied["depot"])
                if isinstance(caravan, dict):
                    copies.extend(value for value in (caravan.get("source_depot"), caravan.get("destination_depot")) if value is not None)
            if isinstance(raid, dict): copies.extend((raid.get("source_depot"), raid.get("camp_depot")))
            for raw in copies:
                if not isinstance(raw, dict) or type(raw.get("id")) is not int or raw["id"] not in self.specs:
                    self.violation("foreign_or_invalid_food_depot", sid); continue
                identity, expected = raw["id"], self.specs[raw["id"]]
                values = {key: counter(raw.get(key, "0" if key in ("stolen", "exported") else None)) for key in DEPOT_COUNTERS}
                if (point(raw.get("position")) is None or raw["position"].get("map_id") != 725 or
                    distance(point(raw["position"]), point(expected["position"])) > .01 or
                    raw.get("capacity") != (120 if identity == 7 else 30) or
                    raw.get("low_threshold") != (12 if identity == 7 else 4) or
                    not quantity(raw.get("stock"), raw["capacity"]) or any(value is None for value in values.values())):
                    self.violation("food_depot_geometry_or_metrics_changed", identity); continue
                normalized = dict(values, stock=raw["stock"])
                if identity in depots and depots[identity] != normalized:
                    self.violation("conflicting_food_depot_copies", identity)
                if normalized["stock"] != values["delivered"]-sum(values[k] for k in ("consumed", "stolen", "exported")):
                    self.violation("warehouse_absolute_food_flow_unreconciled", identity)
                depots[identity] = normalized
        complete = set(depots) == set(self.specs) and set(metrics) == FOOD_ACTORS and accounting is not None
        if not complete:
            self.quality["incomplete_seven_depot_actor_ledger"] += 1
            self.previous_ledger = None
            return depots, metrics, accounting
        self.complete += 1
        if sum(value["stock"] for value in depots.values()) != accounting["in_depots"]:
            self.violation("global_depot_food_total_disagrees")
        if sum(value["cargo"] for value in metrics.values()) != accounting["in_cargo"]:
            self.violation("global_cargo_food_total_disagrees")
        for key in ("harvested", "consumed", "lost"):
            if sum(value[key] for value in metrics.values()) != accounting[key]:
                self.violation("global_actor_food_total_disagrees_"+key)
        for key in ("delivered", "stolen"):
            if sum(value[key] for value in metrics.values()) != sum(value[key] for value in depots.values()):
                self.violation("actor_and_depot_totals_disagree_"+key)
        if sum(value["loaded"] for value in metrics.values()) != sum(value["exported"] for value in depots.values()):
            self.violation("caravan_loading_and_exports_disagree")
        if self.previous_ledger is not None:
            old_depots, old_metrics, old_accounting = self.previous_ledger
            self.audited_intervals += 1
            self.audited_epochs[self.epoch] += 1
            for sid, row in metrics.items():
                if any(row[key] < old_metrics[sid][key] for key in METRICS):
                    self.violation("actor_food_counter_decreased", sid)
            for identity, row in depots.items():
                if any(row[key] < old_depots[identity][key] for key in DEPOT_COUNTERS):
                    self.violation("depot_food_counter_decreased", identity)
                members = [sid for sid, value in metrics.items() if value["depot_id"] == identity]
                for key in ("delivered", "consumed"):
                    if row[key]-old_depots[identity][key] != sum(metrics[sid][key]-old_metrics[sid][key] for sid in members):
                        self.violation("depot_actor_flow_unreconciled_"+key, identity)
                theft = sum(value["stolen"]-old_metrics[sid]["stolen"] for sid, value in metrics.items() if value["source"] == identity)
                loaded = sum(value["loaded"]-old_metrics[sid]["loaded"] for sid, value in metrics.items() if value["caravan_source"] == identity)
                if row["stolen"]-old_depots[identity]["stolen"] != theft:
                    self.violation("theft_source_flow_unreconciled", identity)
                if row["exported"]-old_depots[identity]["exported"] != loaded:
                    self.violation("loading_source_flow_unreconciled", identity)
            if any(accounting[key] < old_accounting[key] for key in ("harvested", "consumed", "lost")):
                self.violation("global_food_counter_decreased")
        self.previous_ledger = depots, metrics, accounting
        return depots, metrics, accounting

    @staticmethod
    def owned(actor, *, moving=False):
        role, movement = actor.get("living_role") or {}, actor.get("movement") or {}
        return (actor.get("action") == ("MOVE_TO" if moving else "AMBIENT") and
            actor.get("action_source_goal") == "LOCAL_ACTIVITY" and actor.get("goal") == "LOCAL_ACTIVITY" and
            actor.get("goal_owner") == "INDIVIDUAL" and common.positive_int(actor.get("action_started_at_ms")) and
            actor["action_started_at_ms"] == role.get("started_at_ms") and
            role.get("phase") == ("MOVING" if moving else "ACTING") and
            movement.get("moving") is moving and actor.get("in_combat") is False and
            ((point(actor.get("destination")) is not None and actor["destination"].get("map_id") == 725)
             if moving else role.get("activity") == "WORK"))

    def pair_rows(self, actors, identity, capture):
        pair, rows, shared = PAIRS[identity], [], None
        roster = (pair["carrier_spawn_id"], pair["escort_spawn_id"])
        for sid in roster:
            actor = actors.get(sid, {})
            supplied, role, position = actor.get("supply") or {}, actor.get("living_role") or {}, actor.get("position") or {}
            caravan = supplied.get("caravan")
            if (not isinstance(caravan, dict) or actor.get("alive") is not True or not finite(actor.get("health")) or actor["health"] <= 0 or
                position.get("source") != "live" or position.get("map_id") != 725 or point(position) is None or
                not isinstance(actor.get("runtime_guid"), str) or not actor["runtime_guid"] or
                not common.positive_int(role.get("lifetime_ms")) or role["lifetime_ms"] > capture):
                return None
            counters = {key: counter(supplied.get(key, "0" if key == "stolen" else None)) for key in METRICS if key != "loaded"}
            counters["loaded"] = counter(caravan.get("loaded"))
            trip, completed = counter(caravan.get("trip")), counter(caravan.get("completed_trips"))
            source = caravan.get("source_depot_id")
            carrier = sid == roster[0]
            if (type(caravan.get("id")) is not int or caravan.get("id") != identity or caravan.get("leader_spawn_id") != str(roster[0]) or
                caravan.get("member_spawn_ids") != list(map(str, roster)) or source not in pair["source_depot_ids"] or
                type(source) is not int or type(caravan.get("destination_depot_id")) is not int or
                caravan.get("destination_depot_id") != 7 or trip is None or completed is None or
                not quantity(caravan.get("waypoint"), 20) or type(caravan.get("member_ready")) is not bool or
                caravan.get("stage") not in ("RALLY", "TO_VILLAGE", "LOADING", "TO_CITY", "UNLOADING", "REST", "RECOVER") or
                supplied.get("role") != ("CARAVAN_CARRIER" if carrier else "CARAVAN_ESCORT") or
                type(supplied.get("depot_id")) is not int or supplied.get("depot_id") != 7 or
                type(supplied.get("capacity")) is not int or supplied.get("capacity") != (6 if carrier else 0) or
                not quantity(supplied.get("cargo"), 6 if carrier else 0) or any(value is None for value in counters.values()) or
                actor.get("control_mode") != "AI_WORLD_CONTROLLED" or type(actor.get("world_faction")) is not int or actor.get("world_faction") != 1):
                self.violation("caravan_pair_or_capability_changed", sid); return None
            signature = (source, trip, caravan["waypoint"], caravan["stage"], completed)
            if shared is not None and shared != signature:
                self.violation("conflicting_caravan_pair_copies", identity); return None
            shared = signature
            source_copy, city_copy, own_copy = caravan.get("source_depot"), caravan.get("destination_depot"), supplied.get("depot")
            accounting = supplied.get("accounting")
            if (not all(isinstance(copy, dict) for copy in (source_copy, city_copy, own_copy, accounting)) or
                source_copy.get("id") != source or city_copy.get("id") != 7 or own_copy.get("id") != 7 or
                any(counter(copy.get(key, "0")) is None for copy in (source_copy, city_copy) for key in DEPOT_COUNTERS) or
                any(counter(accounting.get(key)) is None for key in common.ACCOUNTING) or accounting.get("conserved") is not True):
                self.quality["missing_local_caravan_ledger"] += 1; return None
            if (counters["harvested"] or counters["stolen"] or
                not carrier and any(counters[k] for k in ("loaded", "delivered", "lost"))):
                self.violation("caravan_unassigned_food_capability", sid)
            rows.append(dict(actor=actor, supplied=supplied, caravan=caravan, counters=counters,
                position=point(position), binding=(actor["runtime_guid"], role["lifetime_ms"]), capture=capture))
        return rows

    def activity(self, row, stage, source):
        anchor = point(self.specs[source if stage == "LOADING" else 7]["position"])
        return (row["caravan"]["stage"] == stage and self.owned(row["actor"]) and
                distance(row["position"], anchor) <= RADIUS and row["actor"]["living_role"]["started_at_ms"] <= row["capture"])

    def event(self, old, row, kind, amount):
        identity, source = row["caravan"]["id"], row["caravan"]["source_depot_id"]
        stage = "LOADING" if kind == "loaded" else "UNLOADING"
        target = point(self.specs[source if kind == "loaded" else 7]["position"])
        actor, role = old["actor"], old["actor"].get("living_role") or {}
        if (amount <= 0 or amount > 6 or not self.activity(old, stage, source) or
            row["capture"]-role.get("started_at_ms", row["capture"]) < 5000):
            self.violation(kind+"_without_owned_warehouse_action", identity); return None
        departure = self.post_load_departure(old, row, target, role["started_at_ms"]+5000) if kind == "loaded" else None
        if ((distance(row["position"], target) > RADIUS and departure is None) or
            row["actor"].get("in_combat") is not False):
            # A legitimate action may finish and hand over to movement/safety
            # between captures. That boundary lacks a physical end receipt;
            # it is missing evidence, not proof that the transfer was remote.
            self.quality[kind+"_completion_position_not_observed"] += 1; return None
        stamp = (row["binding"], role["started_at_ms"], kind)
        if stamp in self.spent:
            self.violation("replayed_caravan_activity_receipt", identity); return None
        self.spent.add(stamp)
        observed = self.activities.get((identity, stage))
        if not observed or observed["stamp"] != stamp[:2] or not observed["start_observed"]:
            self.quality[kind+"_activity_start_not_observed"] += 1; return None
        receipt = dict(started_at_ms=role["started_at_ms"], completion_capture_ms=row["capture"],
                       portions=amount, position_before=list(old["position"]), position_after=list(row["position"]))
        if distance(row["position"], target) > RADIUS:
            receipt["carrier_departure"] = departure
            self.quality["owned_carrier_departure_after_load"] += 1
        return receipt

    def post_load_departure(self, previous, current, anchor, not_before):
        actor, old_actor = current["actor"], previous["actor"]
        started = actor.get("action_started_at_ms")
        if (distance(previous["position"], anchor) > RADIUS or
            old_actor.get("movement", {}).get("moving") is not False or old_actor.get("in_combat") is not False or
            previous["binding"] != current["binding"] or
            any(previous["caravan"][key] != current["caravan"][key] for key in ("id", "trip", "source_depot_id")) or
            current["caravan"]["stage"] != "TO_CITY" or
            not 0 < current["capture"]-previous["capture"] <= self.max_gap*1000 or
            not self.owned(actor, moving=True) or
            actor.get("living_role", {}).get("movement_purpose") != "CARAVAN_CITY" or
            not (not_before <= started <= current["capture"]) or started < previous["capture"]):
            return None
        speed = actor.get("movement", {}).get("run_speed", 7)
        speed = min(speed, 14) if finite(speed) and speed > 0 else 7
        if distance(previous["position"], current["position"]) > speed*(current["capture"]-started)/1000*1.5+.5:
            return None
        return dict(last_warehouse_capture_ms=previous["capture"], movement_started_at_ms=started,
            departure_capture_ms=current["capture"], position_before=list(previous["position"]),
            position_after=list(current["position"]))

    def escort_at_load(self, previous, current, source, receipt):
        anchor = point(self.specs[source]["position"])
        if distance(current["position"], anchor) <= RADIUS:
            return True
        # Loading can finish and hand the escort its return leg between two
        # captures. Keep the observed warehouse presence, exact original life
        # and owned post-load departure; never infer an absent escort.
        old_actor = previous["actor"]
        departure = self.post_load_departure(previous, current, anchor, receipt["started_at_ms"]+5000)
        if (old_actor.get("action") is not None or old_actor.get("living_role", {}).get("phase") != "IDLE" or
            departure is None):
            return False
        receipt["escort_departure"] = departure
        self.quality["owned_escort_departure_after_load"] += 1
        return True

    def update_pair(self, identity, rows, depots):
        old = self.previous_pairs.get(identity)
        carrier, escort = rows
        capture, caravan = carrier["capture"], carrier["caravan"]
        source, trip_id = caravan["source_depot_id"], counter(caravan["trip"])
        bindings = tuple(row["binding"] for row in rows)
        key = self.epoch, identity, trip_id
        track = self.trips.get(identity)
        if track is not None and (track.identity != key or track.bindings != bindings or track.source != source):
            self.quality["pair_trip_or_incarnation_changed"] += 1
            self.trips.pop(identity, None); track = None
        if old and tuple(row["binding"] for row in old) != bindings:
            old = None
            self.activities.pop((identity, "LOADING"), None); self.activities.pop((identity, "UNLOADING"), None)
        if track is None and trip_id and caravan["stage"] in ("RALLY", "TO_VILLAGE") and all(
            distance(row["position"], point(self.specs[7]["position"])) <= RADIUS and
            row["actor"].get("movement", {}).get("moving") is False for row in rows):
            track = Trip(key, source, bindings, capture)
            self.trips[identity] = track
        if old:
            for index, row in enumerate(rows):
                previous, actor = old[index], old[index]["actor"]
                traveled = distance(previous["position"], row["position"])
                speed = actor.get("movement", {}).get("run_speed", 7)
                speed = min(speed, 14) if finite(speed) and speed > 0 else 7
                if traveled > speed*(capture-previous["capture"])/1000*1.5+3:
                    self.violation("caravan_unphysical_position_jump", identity)
                if track is not None and self.owned(actor, moving=True):
                    purpose = (actor.get("living_role") or {}).get("movement_purpose")
                    bucket = track.outward if purpose == "CARAVAN_VILLAGE" else track.returning if purpose == "CARAVAN_CITY" else None
                    if bucket is not None: bucket[index] = bucket.get(index, 0)+traveled
            if track is not None and distance(rows[0]["position"], rows[1]["position"]) <= 40:
                for kind, purpose in (("joint_outward", "CARAVAN_VILLAGE"), ("joint_return", "CARAVAN_CITY")):
                    if all(self.owned(row["actor"], moving=True) and
                        row["actor"].get("living_role", {}).get("movement_purpose") == purpose for row in old):
                        setattr(track, kind, getattr(track, kind)+1)
            delta = {kind: carrier["counters"][kind]-old[0]["counters"][kind] for kind in METRICS}
            if any(value < 0 for value in delta.values()): self.violation("carrier_counter_decreased", identity)
            if carrier["supplied"]["cargo"]-old[0]["supplied"]["cargo"] != delta["loaded"]-delta["delivered"]-delta["lost"]:
                self.violation("carrier_cargo_transfer_unreconciled", identity)
            if delta["harvested"] or delta["stolen"]:
                self.violation("carrier_used_hunt_or_theft_capability", identity)
            if delta["loaded"]:
                receipt = self.event(old[0], carrier, "loaded", delta["loaded"])
                previous_source = old[0]["caravan"]["source_depot"]
                current_source = caravan["source_depot"]
                if counter(current_source.get("exported", "0"))-counter(previous_source.get("exported", "0")) != delta["loaded"]:
                    self.violation("load_receipt_not_debited_from_source", identity)
                if source in depots:
                    # Other consumers/thefts may legitimately spend the
                    # reserve after loading. Add back their actual interval
                    # debits; fail only if even that upper bound is too low.
                    old_raw = old[0]["caravan"].get("source_depot") or {}
                    later_debits = sum(depots[source][k]-counter(old_raw.get(k, "0")) for k in ("consumed", "stolen"))
                    if depots[source]["stock"]+later_debits < self.specs[source].get("low_threshold", 4):
                        self.violation("caravan_consumed_village_reserve", identity)
                if (receipt is not None and track is not None and track.joint_outward >= 2 and
                    all(track.outward.get(i, 0) >= 10 and i in track.outward_targets and
                        distance(track.outward_targets[i], point(self.specs[source]["position"])) <= RADIUS for i in (0, 1)) and
                    (distance(carrier["position"], point(self.specs[source]["position"])) <= RADIUS or "carrier_departure" in receipt) and
                    self.escort_at_load(old[1], escort, source, receipt)):
                    track.load = receipt
            if delta["delivered"]:
                receipt = self.event(old[0], carrier, "delivered", delta["delivered"])
                if counter(caravan["destination_depot"]["delivered"])-counter(old[0]["caravan"]["destination_depot"]["delivered"]) < delta["delivered"]:
                    self.violation("delivery_receipt_not_credited_to_city", identity)
                if receipt is not None and track is not None and track.load is not None and not track.closed:
                    track.delivered += delta["delivered"]
                    if (track.delivered == track.load["portions"] and carrier["supplied"]["cargo"] == 0 and
                        track.joint_return >= 2 and all(track.returning.get(i, 0) >= 10 for i in (0, 1)) and
                        all(i in track.return_targets and distance(track.return_targets[i], point(self.specs[7]["position"])) <= RADIUS for i in (0, 1)) and
                        all(distance(row["position"], point(self.specs[7]["position"])) <= RADIUS for row in rows)):
                        self.receipts.append(dict(epoch=self.epoch, caravan_id=identity, trip=str(trip_id),
                            source_depot_id=source, destination_depot_id=7,
                            member_spawn_ids=[pair for pair in caravan["member_spawn_ids"]],
                            original_bindings=[list(binding) for binding in bindings], assembly_capture_ms=track.assembled_at,
                            loaded=track.load, unloaded=receipt, portions=track.delivered,
                            completed_capture_ms=capture, outward_distances=track.outward, return_distances=track.returning))
                        track.closed = True
        if track is not None:
            for index, row in enumerate(rows):
                if self.owned(row["actor"], moving=True):
                    purpose = row["actor"].get("living_role", {}).get("movement_purpose")
                    targets = track.outward_targets if purpose == "CARAVAN_VILLAGE" else track.return_targets if purpose == "CARAVAN_CITY" else None
                    if targets is not None: targets[index] = point(row["actor"]["destination"])
        for stage in ("LOADING", "UNLOADING"):
            slot = identity, stage
            if self.activity(carrier, stage, source):
                stamp = carrier["binding"], carrier["actor"]["living_role"]["started_at_ms"]
                if slot not in self.activities or self.activities[slot]["stamp"] != stamp:
                    self.activities[slot] = dict(stamp=stamp,
                        start_observed=0 <= capture-stamp[1] <= self.max_gap*1000)
            else:
                self.activities.pop(slot, None)
        self.previous_pairs[identity] = rows

    def observe(self, record):
        usable = self.usable(record)
        if usable is None: return
        state, capture = usable
        agents = state["agents"]
        if (any(not isinstance(actor, dict) or not common.positive_int(actor.get("spawn_id")) for actor in agents) or
            len({actor["spawn_id"] for actor in agents}) != len(agents)):
            self.censor("invalid_or_duplicate_actor_population"); return
        actors = {actor["spawn_id"]: actor for actor in agents}
        if set(actors)-full.SPAWN_IDS: self.violation("foreign_authored_actor")
        if set(actors) != full.SPAWN_IDS: self.quality["incomplete_100_actor_population"] += 1
        depots, _, _ = self.ledger(actors)
        for identity in PAIRS:
            rows = self.pair_rows(actors, identity, capture)
            if rows is None:
                self.quality["missing_dead_or_unloaded_caravan_pair"] += 1
                self.previous_pairs.pop(identity, None); self.trips.pop(identity, None)
                for stage in ("LOADING", "UNLOADING"): self.activities.pop((identity, stage), None)
                continue
            self.update_pair(identity, rows, depots)

    def report(self):
        qualified = []
        for identity, window in self.epochs.items():
            receipts = [receipt for receipt in self.receipts if receipt["epoch"] == identity]
            sources = {receipt["source_depot_id"] for receipt in receipts}
            duration = (window["last_capture_ms"]-window["first_capture_ms"])/1000
            qualified.append((len(sources), duration, identity, receipts, sources))
        selected = max(qualified, default=(0, 0, None, [], set()))
        missing = [f"physical_escorted_delivery_from_village_{source}" for source in sorted(SOURCES-selected[4])]
        if selected[1] < self.minimum_seconds: missing.append("continuous_fresh_observation_window")
        if not self.audited_epochs[selected[2]]: missing.append("complete_consecutive_seven_depot_ledger_interval")
        if self.process is None: missing.append("externally_verified_unchanged_actual_world_process")
        status = "FAIL" if self.violations else "INCONCLUSIVE" if missing else "PASS"
        whole = "PASS" if self.unique and self.complete == self.unique and not self.violations else "INCONCLUSIVE"
        return dict(profile=full.PROFILE, feature="physical_food_caravans", status=status, exit_code=EXIT_CODES[status],
            samples=self.samples, unique_frames=self.unique, selected_epoch=selected[2], continuous_seconds=selected[1],
            minimum_seconds=self.minimum_seconds, max_gap_seconds=self.max_gap,
            complete_ledger_frames=self.complete, audited_ledger_intervals=self.audited_intervals,
            selected_epoch_audited_ledger_intervals=self.audited_epochs[selected[2]],
            whole_record_food_ledger_status="FAIL" if self.violations else whole,
            required_source_depot_ids=sorted(SOURCES), witnessed_source_depot_ids=sorted(selected[4]),
            qualifying_receipts=selected[3], receipts=self.receipts, quality=dict(self.quality),
            violations=list(self.violations.values()), missing_evidence=missing,
            process_identity_verified=self.process is not None,
            limitations=["PASS certifies linked physical escorted load, return and city delivery for all four villages in one fresh observation epoch.",
                "Missing dead/unloaded actors are never replaced with frozen or inferred food. Their intervals are censored from full-ledger auditing; independent fully observed caravan trips may still qualify.",
                "The actual-world process fingerprint is supplied by an external collector; telemetry timestamps alone cannot prove a process identity.",
                "This transport gate does not independently certify farming, hunting or theft behavior that originally produced or moved the food.",
                "Position/action observations do not independently prove map collision, line of sight or client animation pixels."])


def evaluate_samples(samples, *, minimum_seconds=300, max_gap_seconds=2, process_proof=None):
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds, process_proof=process_proof)
    for record in samples: evaluator.observe(record)
    return evaluator.report()


def verify_part_hashes(path, proof):
    """Bind a closed collector process claim to its exact immutable recording."""
    parts = proof.get("parts") if isinstance(proof, dict) else None
    if not isinstance(parts, list) or not parts: raise ValueError("Process proof needs hashed recording parts")
    root = path if path.is_dir() else path.parent
    expected = {}
    for part in parts:
        if not isinstance(part, dict) or not isinstance(part.get("name"), str) or Path(part["name"]).name != part["name"]:
            raise ValueError("Invalid process-proof part name")
        name = part["name"]
        if name in expected or "/" in name or "\\" in name: raise ValueError("Invalid duplicate process-proof part")
        expected[name] = part.get("sha256")
    actual_parts = sorted(root.glob("part-*.jsonl*")) if path.is_dir() else [path]
    if {part.name for part in actual_parts} != set(expected): raise ValueError("Process proof does not cover exactly the recording")
    for part in actual_parts:
        if hashlib.sha256(part.read_bytes()).hexdigest() != expected[part.name]: raise ValueError("Recording part hash differs from process proof")
    if proof.get("status") != "capture_finished" or valid_process(proof) is None:
        raise ValueError("Process proof must describe a closed unchanged actual process")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=300)
    parser.add_argument("--max-gap-seconds", type=float, default=2)
    parser.add_argument("--process-proof", type=Path, help="closed collector metadata with actual process identity and all part hashes")
    args = parser.parse_args(argv)
    try:
        proof = json.loads(args.process_proof.read_text(encoding="utf-8-sig")) if args.process_proof else None
        if proof is not None: verify_part_hashes(args.path, proof)
        report = evaluate_samples(common.recording_rows(args.path), minimum_seconds=args.minimum_seconds,
            max_gap_seconds=args.max_gap_seconds, process_proof=proof)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile=full.PROFILE, feature="physical_food_caravans", status="INCONCLUSIVE", exit_code=2,
            missing_evidence=["readable_closed_recording_and_matching_process_proof"], input_error=str(error))
    rendered = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)+"\n"
    if args.output: args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
