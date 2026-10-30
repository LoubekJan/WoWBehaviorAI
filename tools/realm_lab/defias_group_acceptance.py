"""Read-only proof of a physical Defias party trip over the full food ledger.

PASS certifies an observed assembly, owned joint outward travel, a shared
warehouse visit or witnessed guard interception, and linked returns/deaths.
It does not certify theft success, formation pixels, or memory causality.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path

try:
    from . import defias_population as population, field_acceptance as fields, full_population as full, telemetry_contract
except ImportError:
    import defias_population as population
    import field_acceptance as fields
    import full_population as full
    import telemetry_contract

PHASES = frozenset(("ASSEMBLING", "OUTBOUND", "RAIDING", "RETURNING", "COMPLETE"))
SOURCES = frozenset((1, 4, 5, 6))
ASSEMBLY_RADIUS = 8.01
JOINT_RADIUS = 60.0
MINIMUM_TRAVEL = 10.0
MAX_TRACKS = 64
PROFILES = (population.PROFILE, full.PROFILE)


@dataclass
class PartyTrack:
    identity: tuple
    roster: tuple
    leader: int
    source: int
    route: int
    assembled_at: int
    bindings: dict
    previous: dict = field(default_factory=dict)
    outward: dict = field(default_factory=dict)
    returning: dict = field(default_factory=dict)
    joint_members: set = field(default_factory=set)
    objective: dict = field(default_factory=dict)
    terminal: dict = field(default_factory=dict)
    memories: dict = field(default_factory=dict)
    closed: bool = False


class _Evaluator:
    def __init__(self, minimum_seconds=300, max_gap_seconds=2, *, profile=population.PROFILE):
        if profile not in PROFILES:
            raise ValueError("Defias party acceptance requires an explicit defias-v8 or full-v8 profile")
        self.profile = profile
        selected = full if profile == full.PROFILE else population
        self.field_gate = fields._Evaluator(minimum_seconds, max_gap_seconds,
            population_spec=selected.Population.load(), raider_ids=population.RAIDER_IDS,
            camp_members=population.DEFIAS_IDS, profile=profile)
        self.minimum_seconds, self.max_gap = minimum_seconds, max_gap_seconds
        self.tracks, self.receipts = {}, []
        self.retired_bindings = {}
        self.current_death_bindings = {}
        self.frozen_dead_camp_metrics = {}
        self.camp_previous = {}
        self.party_frames = 0

    def violation(self, code, sid=0):
        self.field_gate.violation(code, sid)

    def party(self, actor):
        raid = actor.get("raid") or {}
        party = raid.get("party")
        if party is None:
            return None
        if not isinstance(party, dict):
            self.violation("invalid_defias_party", actor["spawn_id"]); return None
        raw = party.get("member_spawn_ids")
        identity, leader = fields.counter(party.get("id")), fields.counter(party.get("leader_spawn_id"))
        members = [fields.counter(value) for value in raw] if isinstance(raw, list) else []
        if (not identity or leader is None or not 1 <= len(members) <= 5 or
            any(not member for member in members) or len(set(members)) != len(members) or
            not set(members).issubset(population.RAIDER_IDS) or leader and leader not in members or
            party.get("phase") not in PHASES or actor["spawn_id"] not in members or
            party.get("role") != ("LEADER" if actor["spawn_id"] == leader else "MEMBER") or
            raid.get("role") != "RAIDER"):
            self.violation("invalid_defias_party", actor["spawn_id"]); return None
        planning = raid.get("planning") or {}
        attempt, route = fields.counter(raid.get("attempt_id")), planning.get("route_id")
        source = raid.get("source_depot_id")
        if attempt is None or type(source) is not int or source not in SOURCES or type(route) is not int or route not in (1, 2):
            self.violation("invalid_defias_party_objective", actor["spawn_id"]); return None
        return dict(id=identity, leader=leader, members=tuple(members), phase=party["phase"],
                    source=source, route=route, attempt=attempt)

    @staticmethod
    def owned_move(actor, purpose):
        role, movement = actor.get("living_role") or {}, actor.get("movement") or {}
        return (actor.get("action") == "MOVE_TO" and actor.get("action_source_goal") == "LOCAL_ACTIVITY" and
            actor.get("goal") == "LOCAL_ACTIVITY" and actor.get("goal_owner") == "INDIVIDUAL" and
            fields.supply.positive_int(actor.get("action_started_at_ms")) and
            actor["action_started_at_ms"] == role.get("started_at_ms") and role.get("phase") == "MOVING" and
            role.get("movement_purpose") in purpose and movement.get("moving") is True and actor.get("in_combat") is False)

    def actor_row(self, actor, capture, party):
        role, supplied = actor.get("living_role") or {}, actor.get("supply") or {}
        position = fields.point(actor.get("position"))
        dead = actor.get("alive") is False and actor.get("health") == 0
        binding = self.current_death_bindings.get(actor["spawn_id"]) if dead else None
        if binding is None and fields.supply.positive_int(role.get("lifetime_ms")):
            binding = (actor.get("runtime_guid"), role["lifetime_ms"])
        if (position is None or actor.get("position", {}).get("source") != "live" or
            not actor.get("runtime_guid") or binding is None or binding[1] > capture):
            return None
        if (supplied.get("enabled") is not True or supplied.get("role") != "RAIDER" or
            supplied.get("depot_id") != 3 or supplied.get("capacity") != 2 or
            not fields.quantity(supplied.get("cargo"), 2) or role.get("role") != "RAIDER" or
            role.get("enabled") is not True):
            self.violation("defias_raider_capability_changed", actor["spawn_id"]); return None
        camp, source = self.field_gate.depots[3], self.field_gate.depots[party["source"]]
        return dict(actor=actor, position=position, capture=capture,
            binding=binding, observed_role_lifetime_ms=role.get("lifetime_ms"),
            camp_distance=fields.dist3(position, fields.point(camp)),
            source_distance=fields.dist3(position, fields.point(source)),
            outward=self.owned_move(actor, {"RAID_SOURCE"}),
            returning=self.owned_move(actor, {"RAID_CAMP", "RAID_EMPTY_RETURN"}),
            alive=actor.get("alive") is True and isinstance(actor.get("health"), (int, float)) and actor["health"] > 0,
            dead=dead)

    def death_bindings(self, record):
        """Retain a prior live identity only for a directly witnessed corpse.

        Death clears LivingRole in the runtime, so its raw lifetime can be zero.
        The old positive lifetime is provenance from the immediately preceding
        fresh bound sample, never a fabricated postmortem role observation.
        """
        result = dict(self.retired_bindings)
        state = record if isinstance(record, dict) and "agents" in record else (record.get("state") if isinstance(record, dict) else None)
        if not isinstance(state, dict) or not isinstance(state.get("agents"), list):
            return result
        capture = state.get("captured_at_ms")
        if not fields.supply.positive_int(capture):
            return result
        for actor in state["agents"]:
            if (not isinstance(actor, dict) or actor.get("spawn_id") not in population.DEFIAS_IDS or
                actor.get("alive") is not False or actor.get("health") != 0 or
                (actor.get("position") or {}).get("source") != "live" or
                (actor.get("position") or {}).get("map_id") != 725):
                continue
            sid, position = actor["spawn_id"], fields.point(actor.get("position"))
            if position is None:
                continue
            if sid not in population.RAIDER_IDS:
                old = self.camp_previous.get(sid)
                if old is None:
                    continue
                original = old["binding"]
            else:
                party = self.party(actor)
                if party is None:
                    continue
                track = self.tracks.get((self.field_gate.epoch, party["id"], party["attempt"]))
                if track is None or track.closed or sid not in track.bindings:
                    continue
                if (track.roster, track.leader, track.source, track.route) != (party["members"], party["leader"], party["source"], party["route"]):
                    continue
                original, old = track.bindings[sid], track.previous.get(sid)
            raw_life = (actor.get("living_role") or {}).get("lifetime_ms")
            if (actor.get("runtime_guid") != original[0] or type(raw_life) is not int or raw_life not in (0, original[1]) or
                old is None or not old["alive"] or not 0 < capture-old["capture"] <= self.max_gap*1000):
                continue
            speed = (old["actor"].get("movement") or {}).get("run_speed", 7)
            speed = min(speed, 14) if fields.finite(speed) and speed > 0 else 7
            if fields.dist3(position, old["position"]) <= speed*(capture-old["capture"])/1000*1.5+3:
                result[sid] = original
        return result

    def validate_defias_capabilities(self, actors):
        for sid in population.DEFIAS_IDS:
            if sid in self.frozen_dead_camp_metrics:
                continue
            actor = actors[sid]
            supplied, role = actor.get("supply") or {}, actor.get("living_role") or {}
            raider = sid in population.RAIDER_IDS
            expected_role = "RAIDER" if raider else "GUARD" if sid in population.GUARD_IDS else "WORKER"
            if (supplied.get("enabled") is not True or supplied.get("role") != ("RAIDER" if raider else "CONSUMER") or
                supplied.get("depot_id") != 3 or supplied.get("capacity") != (2 if raider else 0) or
                not fields.quantity(supplied.get("cargo"), 2 if raider else 0) or
                role.get("enabled") is not True or role.get("role") != expected_role or
                not raider and actor.get("raid") is not None):
                self.violation("defias_runtime_capability_changed", sid)

    def update(self, party, rows, actors, capture):
        key = (self.field_gate.epoch, party["id"], party["attempt"])
        track = self.tracks.get(key)
        if track is None:
            if (party["phase"] != "ASSEMBLING" or len(rows) < 2 or len(rows) != len(party["members"]) or
                not party["attempt"] or any(not row["alive"] or row["camp_distance"] > ASSEMBLY_RADIUS for row in rows.values())):
                return
            track = self.tracks[key] = PartyTrack(key, party["members"], party["leader"], party["source"],
                party["route"], capture, {sid: row["binding"] for sid, row in rows.items()})
            # A closed recording is bounded; never retain arbitrary unbounded
            # actor-provided party identities as proof state.
            if len(self.tracks) > MAX_TRACKS:
                del self.tracks[next(iter(self.tracks))]
        if track.closed:
            return
        dead_members = {sid for sid, terminal in track.terminal.items() if terminal["kind"] == "DEATH"}
        # A captured death is already a final outcome for that exact binding.
        # Its later respawn may clear PartyId while the surviving old party is
        # still returning. Missing live members remain an evidence break.
        if not (set(track.roster)-dead_members).issubset(rows) or set(rows)-set(track.roster):
            track.closed = True; return
        if (track.roster, track.leader, track.source, track.route) != (
            party["members"], party["leader"], party["source"], party["route"]):
            self.violation("defias_party_objective_changed_mid_attempt"); track.closed = True; return
        for sid, row in rows.items():
            if row["binding"] != track.bindings.get(sid):
                track.closed = True; return
            if sid in dead_members:
                if not row["dead"]:
                    track.closed = True; return
                continue
            old = track.previous.get(sid)
            if old is not None:
                seconds = (capture-old["capture"])/1000
                travel = fields.dist3(row["position"], old["position"])
                speed = (row["actor"].get("movement") or {}).get("run_speed", 7)
                speed = min(speed, 14) if fields.finite(speed) and speed > 0 else 7
                if not 0 < seconds <= self.max_gap or travel > speed*seconds*1.5+3:
                    track.closed = True; return
                if party["phase"] == "OUTBOUND" and old["outward"] and row["outward"]:
                    track.outward[sid] = track.outward.get(sid, 0)+travel
                if party["phase"] in ("RETURNING", "COMPLETE") and old["returning"] and row["returning"]:
                    track.returning[sid] = track.returning.get(sid, 0)+travel
            radius = 1.91 if party["source"] in (4, 5, 6) else 3.01
            if party["phase"] in ("OUTBOUND", "RAIDING") and row["alive"] and row["source_distance"] <= radius:
                track.objective.setdefault(sid, dict(kind="WAREHOUSE_VISIT", capture_ms=capture))
            actor = row["actor"]
            if (actor.get("in_combat") is True and party["phase"] in ("OUTBOUND", "RAIDING", "RETURNING") and
                track.outward.get(sid, 0) >= MINIMUM_TRAVEL and row["camp_distance"] >= MINIMUM_TRAVEL):
                for guard in actors.values():
                    location = fields.point(guard.get("position"))
                    if (guard.get("type") == "GUARD" and guard.get("alive") is True and
                        guard.get("in_combat") is True and guard.get("position", {}).get("source") == "live" and
                        (guard.get("target") or {}).get("spawn_id") == sid and location is not None and
                        fields.dist3(row["position"], location) <= JOINT_RADIUS):
                        track.objective[sid] = dict(kind="GUARD_INTERCEPTION", capture_ms=capture,
                            guard_spawn_id=guard["spawn_id"], position=dict(guard["position"]))
            planning = (actor.get("raid") or {}).get("planning") or {}
            if type(planning.get("memory_records")) is int and planning["memory_records"] > 0:
                track.memories[sid] = dict(capture_ms=capture, records=planning["memory_records"], reason=planning.get("reason"))
            if row["dead"]:
                track.terminal.setdefault(sid, dict(kind="DEATH", capture_ms=capture,
                    runtime_guid=row["binding"][0], lifetime_ms=row["binding"][1],
                    observed_role_lifetime_ms=row["observed_role_lifetime_ms"],
                    prior_alive_capture_ms=old["capture"] if old is not None else None))
                self.retired_bindings[sid] = row["binding"]
            elif sid in track.joint_members:
                if (party["phase"] in ("RETURNING", "COMPLETE") and row["alive"] and
                      row["camp_distance"] <= 3.01 and actor.get("in_combat") is False and
                      (actor.get("supply") or {}).get("cargo") == 0 and track.returning.get(sid, 0) >= 2):
                    track.terminal.setdefault(sid, dict(kind="CAMP_RETURN", capture_ms=capture))
        if party["phase"] == "OUTBOUND":
            travelers = {sid for sid, row in rows.items() if row["alive"] and row["outward"] and
                row["camp_distance"] >= MINIMUM_TRAVEL and track.outward.get(sid, 0) >= MINIMUM_TRAVEL}
            for first in travelers:
                for second in travelers-{first}:
                    if fields.dist3(rows[first]["position"], rows[second]["position"]) <= JOINT_RADIUS:
                        track.joint_members.update((first, second))
        track.previous = {sid: row for sid, row in rows.items() if not row["dead"]}
        witnessed = bool(track.objective.keys() & track.joint_members)
        if len(track.joint_members) >= 2 and set(track.roster).issubset(track.terminal) and witnessed:
            self.receipts.append(dict(epoch=key[0], party_id=str(key[1]), attempt_id=str(key[2]),
                leader_spawn_id=track.leader, member_spawn_ids=list(track.roster), source_depot_id=track.source,
                route_id=track.route, assembly_capture_ms=track.assembled_at, completed_capture_ms=capture,
                joint_travel_members=sorted(track.joint_members), outward_yards=track.outward,
                objective_evidence=track.objective, member_outcomes=track.terminal,
                observed_memory_records=track.memories, theft_success_certified=False))
            track.closed = True

    def observe(self, record):
        prior_frames, prior_epoch = self.field_gate.complete, self.field_gate.epoch
        self.current_death_bindings = self.death_bindings(record)
        state = (record if "agents" in record else record.get("state")) if isinstance(record, dict) else None
        raw_agents = state.get("agents") if isinstance(state, dict) else None
        raw_actors = {actor.get("spawn_id"): actor for actor in raw_agents if isinstance(actor, dict)} if isinstance(raw_agents, list) else {}
        self.frozen_dead_camp_metrics = {sid: previous for sid, binding in self.retired_bindings.items()
            if fields.dead_camp_absence(raw_actors.get(sid)) and
            (previous := (self.field_gate.previous_metrics or {}).get(sid)) is not None and previous["cargo"] == 0 and
            previous["guid"] == binding[0] and previous["life"] in (0, binding[1])}
        self.field_gate.observe(record, witnessed_dead_bindings=self.current_death_bindings,
                                frozen_dead_camp_metrics=self.frozen_dead_camp_metrics)
        if self.field_gate.epoch != prior_epoch:
            self.tracks.clear()
            self.retired_bindings.clear()
            self.current_death_bindings.clear()
            self.frozen_dead_camp_metrics.clear()
            self.camp_previous.clear()
        if (self.profile == full.PROFILE and isinstance(state, dict) and
            state.get("configured") is True and state.get("stale") is False and state.get("version") == 4 and
            fields.supply.positive_int(state.get("captured_at_ms")) and type(state.get("age_ms")) is int and
            0 <= state["age_ms"] < 5000 and ("agents" in record or record.get("status") == "fresh")):
            try:
                # The wider cohort must keep its reviewed ordinary roles and
                # cannot hide unauthorized capability changes behind a valid
                # trip by the unchanged specialized Defias members.
                telemetry_contract.validate_snapshot(state, self.profile)
            except (ValueError, TypeError, KeyError, AttributeError):
                self.violation("full_population_runtime_contract_changed")
        if self.field_gate.complete == prior_frames:
            return
        capture = state["captured_at_ms"]
        actors = {actor["spawn_id"]: actor for actor in state["agents"]}
        self.retired_bindings = {sid: binding for sid, binding in self.retired_bindings.items()
            if self.field_gate.previous_metrics[sid]["guid"] == binding[0] and
            self.field_gate.previous_metrics[sid]["life"] in (0, binding[1])}
        self.validate_defias_capabilities(actors)
        # Camp residents contribute to the shared food ledger, but are never
        # party travelers or party death outcomes. Retire only directly seen
        # deaths linked to their immediately preceding fresh live binding.
        for sid in population.DEFIAS_IDS-population.RAIDER_IDS:
            actor = actors[sid]
            role, location = actor.get("living_role") or {}, actor.get("position") or {}
            position, guid, life = fields.point(location), actor.get("runtime_guid"), role.get("lifetime_ms")
            if actor.get("alive") is False and actor.get("health") == 0 and sid in self.current_death_bindings:
                self.retired_bindings[sid] = self.current_death_bindings[sid]
            if (actor.get("alive") is True and type(actor.get("health")) in (int, float) and actor["health"] > 0 and
                location.get("source") == "live" and location.get("map_id") == 725 and
                position is not None and isinstance(guid, str) and guid.strip() and
                fields.supply.positive_int(life) and life <= capture):
                self.camp_previous[sid] = dict(actor=actor, position=position, capture=capture,
                                               binding=(guid, life), alive=True)
            else:
                self.camp_previous.pop(sid, None)
        parties = {}
        for sid in population.RAIDER_IDS:
            actor = actors[sid]
            party = self.party(actor)
            if party is None:
                continue
            row = self.actor_row(actor, capture, party)
            if row is None:
                continue
            signature = tuple(party[name] for name in ("leader", "members", "phase", "source", "route", "attempt"))
            if party["id"] in parties and parties[party["id"]][0] != signature:
                self.violation("conflicting_defias_party_copies", sid); continue
            _, _, rows = parties.setdefault(party["id"], (signature, party, {}))
            rows[sid] = row
        if parties:
            self.party_frames += 1
        for _, party, rows in parties.values():
            self.update(party, rows, actors, capture)

    def report(self):
        ledger = self.field_gate.report()
        qualifying = [receipt for receipt in self.receipts if
            (epoch := self.field_gate.epochs.get(receipt["epoch"])) and
            (epoch["last_capture"]-epoch["first_capture"])/1000 >= self.minimum_seconds]
        missing = [] if qualifying else ["assembly_owned_joint_travel_common_objective_and_linked_member_outcomes"]
        if not any((epoch["last_capture"]-epoch["first_capture"])/1000 >= self.minimum_seconds
                   for epoch in self.field_gate.epochs.values()):
            missing.append("shared_continuous_fresh_observation_window")
        violations = ledger["violations"]
        status = "FAIL" if violations else "INCONCLUSIVE" if missing else "PASS"
        return dict(profile=self.profile, status=status, exit_code=fields.EXIT_CODES[status],
            criterion="physical_group_raid_trip_with_individual_member_outcomes",
            expected_owned=len(self.field_gate.managed), expected_native=len(self.field_gate.actors)-len(self.field_gate.managed),
            expected_defias=16, expected_raiders=10, expected_camp_members=6,
            complete_population_frames=self.field_gate.complete, party_frames=self.party_frames,
            minimum_observation_seconds=self.minimum_seconds, max_gap_seconds=self.max_gap,
            receipts=self.receipts, qualifying_receipts=len(qualifying), quality=ledger["quality"],
            violations=violations, missing_evidence=missing,
            limitations=["PASS proves party movement and a warehouse visit or actual guard interception, including legitimate retreat or death. It never implies a successful theft or delivery.",
                "Food conservation and each warehouse's delivery, meal and stolen flows include all sixteen Defias and remain independent.",
                "For a directly witnessed dead camp member subsequently declared ABSTRACT without live observations, zero-cargo actor counters are inferred frozen from its last actual ledger snapshot. No absent position, action or party outcome is fabricated; all six current depot copies and global flows remain required.",
                "Observed memory counts are descriptive. This gate does not certify causal memory-driven target choice; the separate memory proof is required for that claim.",
                "Position and owned-action receipts do not prove client animation pixels or independently measure collision and line of sight."])


def evaluate_samples(samples, *, minimum_seconds=300, max_gap_seconds=2, profile=population.PROFILE):
    evaluator = _Evaluator(minimum_seconds, max_gap_seconds, profile=profile)
    for sample in samples:
        evaluator.observe(sample)
    return evaluator.report()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--minimum-seconds", type=float, default=300)
    parser.add_argument("--max-gap-seconds", type=float, default=2)
    parser.add_argument("--profile", choices=PROFILES, default=population.PROFILE)
    args = parser.parse_args(argv)
    try:
        report = evaluate_samples(fields.recording_rows(args.path), minimum_seconds=args.minimum_seconds,
                                  max_gap_seconds=args.max_gap_seconds, profile=args.profile)
    except (OSError, EOFError, UnicodeError, ValueError, TypeError) as error:
        report = dict(profile=args.profile, status="INCONCLUSIVE", exit_code=2,
                      missing_evidence=["readable_closed_recording"], error=str(error))
    text = json.dumps(report, indent=2, ensure_ascii=False)
    if args.output:
        args.output.write_text(text+"\n", encoding="utf-8")
    print(text)
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
