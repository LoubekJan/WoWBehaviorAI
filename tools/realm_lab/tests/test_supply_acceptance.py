"""Export-shaped v8 observations must prove food flow, not counters alone."""
from contextlib import redirect_stdout
from copy import deepcopy
import gzip
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import supply_acceptance as acceptance

BASE = 2_000_000
TARGETS = {901034: 901088, 901061: 901073, 901062: 901074}
METADATA = acceptance.wildlife.MANIFEST
ACTORS = {a["spawn_id"]: a for a in json.loads(METADATA.read_text(encoding="utf-8"))["actors"]}
DEPOTS = {a["id"]: a["position"] for a in acceptance.population.DEPOTS}
GROUNDS = {a["id"]: a["position"] for a in acceptance.population.GROUNDS}


def xyz(value):
    return {key: value[key] for key in ("x", "y", "z")}


def position(value):
    return dict(map_id=725, **xyz(value))


def move(actor, point, moving=False):
    actor["position"].update(xyz(point))
    actor["movement"].update(moving=moving, move_speed=8 if moving else 0,
        home_distance=math.hypot(point["x"] - actor["home"]["x"], point["y"] - actor["home"]["y"]))


def interpolate(a, b, fraction):
    return {axis: a[axis] + (b[axis] - a[axis]) * fraction for axis in ("x", "y", "z")}


def exported_actor(source):
    sid = source["spawn_id"]
    owned = sid in acceptance.OWNED_IDS
    hunter, predator = sid in acceptance.HUNTER_IDS, sid in acceptance.PREDATOR_IDS
    role = "HUNTER" if hunter else "PREDATOR" if predator else "PREY"
    return dict(agent_id=sid, spawn_id=sid, entry=source["entry"], native_creature=not owned,
        type="COMBATANT" if hunter else role if owned else "UNCLASSIFIED",
        control_mode="AI_WORLD_CONTROLLED" if owned else "OBSERVE_ONLY",
        world_faction=(1 if sid == 901034 else 3) if hunter else 0,
        world_state="MATERIALIZED", position=dict(**position(source["home"]), source="live"),
        home=position(source["home"]), runtime_guid=f"Creature-725-{sid}", alive=True, health=100, max_health=100,
        in_combat=False, needs=dict(hunger=0.8) if owned else None, economy=None,
        movement=dict(home_distance=0, moving=False, run_speed=8, move_speed=0),
        living_role=dict(enabled=True, role=role, phase="IDLE", activity="NONE", started_at_ms=0,
                         lifetime_ms=BASE - 1000, movement_purpose="NONE") if owned else None,
        supply=None, action=None, action_source_goal=None, target=None)


def happy_samples():
    """Three different real v8 homes/targets, finite meat, 40-second journeys."""
    rows = []
    for t in range(102):
        harvested = 18 if t >= 48 else 0
        delivered = 18 if t >= 94 else 0
        consumed = 3 if t >= 100 else 0
        balance = dict(initial_food="0", harvested=str(harvested), in_depots=str(delivered - consumed),
                       in_cargo=str(harvested - delivered), consumed=str(consumed), lost="0", conserved=True)
        actors = {sid: exported_actor(source) for sid, source in ACTORS.items()}
        for target in TARGETS.values():
            if t >= 43:
                actors[target].update(alive=False, health=0, living_role=None)
        for sid in acceptance.PREDATOR_IDS:
            actors[sid]["supply"] = dict(enabled=True, role="PREDATOR", phase="IDLE", depot_id=0,
                cargo=0, capacity=0, carcass_remaining=None, carcass_spawn_id=None, carcass_life=None,
                hunting_ground=None, harvested="0", delivered="0", consumed="0", lost="0", last_result="NONE",
                depot=None, accounting=deepcopy(balance))
        for sid, target in TARGETS.items():
            actor = actors[sid]
            depot_id = 1 if sid == 901034 else 2
            members = 1 if depot_id == 1 else 2
            stock = members * (6 if t >= 94 else 0) - (members if t >= 100 else 0)
            supply = dict(enabled=True, role="HUNTER", phase="TO_HUNTING_GROUND", depot_id=depot_id,
                cargo=6 if 48 <= t < 94 else 0, capacity=6,
                harvested="6" if t >= 48 else "0", delivered="6" if t >= 94 else "0",
                consumed="1" if t >= 100 else "0", lost="0", last_result="NONE",
                carcass_remaining=(0 if t >= 48 else 6) if t >= 43 else None,
                carcass_spawn_id=str(target) if t >= 43 else None, carcass_life="1" if t >= 43 else None,
                hunting_ground=deepcopy(GROUNDS[depot_id]),
                depot=dict(id=depot_id, name="villageBottom" if depot_id == 1 else "gnolls",
                    position=deepcopy(DEPOTS[depot_id]), stock=stock, capacity=30, low_threshold=4,
                    delivered=str(members * (6 if t >= 94 else 0)), consumed=str(members if t >= 100 else 0)),
                accounting=deepcopy(balance))
            actor["supply"] = supply
            home, meal = ACTORS[sid]["home"], ACTORS[target]["home"]
            if t <= 40:
                move(actor, interpolate(home, meal, t / 40), t not in (0, 40))
                actor.update(action="MOVE_TO", action_source_goal="LOCAL_ACTIVITY")
                actor["living_role"].update(phase="MOVING", movement_purpose="TO_HUNTING_GROUND")
            elif t < 43:
                move(actor, meal)
                supply["phase"] = "HUNTING"
                actor.update(action="ATTACK", action_source_goal="HUNT",
                    target=dict(spawn_id=target, position=position(meal)))
                actor["living_role"].update(phase="HUNTING", started_at_ms=BASE + 41000)
            elif t < 48:
                move(actor, meal)
                supply["phase"] = "HARVESTING"
                actor.update(action="EAT", action_source_goal="FEED", target=dict(spawn_id=target, position=position(meal)))
                actor["living_role"].update(phase="FEEDING", started_at_ms=BASE + 43000)
            elif t == 48:
                move(actor, meal)
                supply.update(phase="CARRYING", last_result="HARVESTED")
                actor["living_role"].update(phase="ACTING", activity="REST")
            elif t < 89:
                move(actor, interpolate(meal, DEPOTS[depot_id], (t - 48) / 40), t < 88)
                supply["phase"] = "RETURNING_TO_DEPOT"
                actor.update(action="MOVE_TO", action_source_goal="LOCAL_ACTIVITY")
                actor["living_role"].update(phase="MOVING", movement_purpose="DELIVER_FOOD")
            elif t < 94:
                move(actor, DEPOTS[depot_id])
                supply["phase"] = "DELIVERING"
                actor.update(action="AMBIENT", action_source_goal="LOCAL_ACTIVITY")
                actor["living_role"].update(phase="ACTING", activity="WORK", started_at_ms=BASE + 89000)
            elif t < 95:
                move(actor, DEPOTS[depot_id])
                supply.update(phase="WAITING", last_result="DELIVERED")
            elif t < 100:
                move(actor, DEPOTS[depot_id])
                supply["phase"] = "EATING_DEPOT"
                actor.update(action="AMBIENT", action_source_goal="LOCAL_ACTIVITY")
                actor["living_role"].update(phase="ACTING", activity="EAT", started_at_ms=BASE + 95000)
            else:
                move(actor, DEPOTS[depot_id])
                supply.update(phase="WAITING", last_result="ATE_DEPOT")
                actor["needs"]["hunger"] = 0
        rows.append(dict(kind="sample", sequence=t + 1, elapsed_seconds=t, status="fresh",
                         state=dict(version=4, configured=True, stale=False, age_ms=0,
                                    captured_at_ms=BASE + t * 1000, agents=list(actors.values()))))
    return rows


def actor(row, sid):
    return next(a for a in row["state"]["agents"] if a["spawn_id"] == sid)


def rebalance(rows):
    for row in rows:
        supplies = {sid: actor(row, sid)["supply"] for sid in acceptance.HUNTER_IDS}
        stocks = {}
        for depot_id, members in ((1, (901034,)), (2, (901061, 901062))):
            delivered = sum(int(supplies[sid]["delivered"]) for sid in members)
            consumed = sum(int(supplies[sid]["consumed"]) for sid in members)
            stocks[depot_id] = delivered - consumed
            for sid in members:
                supplies[sid]["depot"].update(stock=delivered - consumed, delivered=str(delivered), consumed=str(consumed))
        balance = dict(initial_food="0", harvested=str(sum(int(s["harvested"]) for s in supplies.values())),
            in_depots=str(sum(stocks.values())), in_cargo=str(sum(s["cargo"] for s in supplies.values())),
            consumed=str(sum(int(s["consumed"]) for s in supplies.values())), lost="0", conserved=True)
        for observed in row["state"]["agents"]:
            if observed["supply"] is not None:
                observed["supply"]["accounting"] = deepcopy(balance)


class SupplyAcceptanceTests(unittest.TestCase):
    def test_three_observed_cycles_and_a_physical_meal_pass(self):
        report = acceptance.evaluate_samples(happy_samples())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["delivered_hunters"], sorted(acceptance.HUNTER_IDS))
        self.assertEqual(len(report["cycles"]), 3)
        self.assertEqual(len(report["meals"]), 3)
        for cycle in report["cycles"]:
            self.assertEqual(cycle["covered"], list(acceptance.PHASES))
            self.assertGreater(cycle["carry_travel_yards"], 100)
            self.assertEqual(cycle["delivered_portions"], 6)
            self.assertLessEqual(cycle["delivery_distance_yards"], 3)
        self.assertEqual(report["accounting"]["harvested"], 18)

    def test_api_states_and_identical_transport_polls_use_the_same_evidence(self):
        rows = happy_samples()
        expected = acceptance.evaluate_samples(rows)
        self.assertEqual(acceptance.evaluate_samples(row["state"] for row in rows)["status"], "PASS")
        duplicate = deepcopy(rows[45]); duplicate["elapsed_seconds"] += 0.1
        duplicate["state"]["age_ms"] = 100
        rows.insert(46, duplicate)
        for index, row in enumerate(rows, 1):
            row["sequence"] = index
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["cycles"], expected["cycles"])
        self.assertEqual(report["fresh_samples"], expected["fresh_samples"])
        self.assertEqual(report["quality"]["identical_capture_ignored"], 1)

    def test_missing_hunt_death_harvest_delivery_or_meal_never_passes(self):
        for missing in ("HUNTING", "TARGET_DEATH", "HARVESTING", "DELIVERING", "EATING_DEPOT"):
            with self.subTest(missing=missing):
                rows = happy_samples()
                for row in rows:
                    hunter = actor(row, 901034)
                    if hunter["supply"]["phase"] == missing:
                        hunter["supply"]["phase"] = "WAITING"
                    if missing == "TARGET_DEATH" and row["elapsed_seconds"] < 43:
                        actor(row, TARGETS[901034]).update(alive=False, health=0, living_role=None)
                    if missing == "EATING_DEPOT":
                        for sid in acceptance.HUNTER_IDS:
                            if actor(row, sid)["supply"]["phase"] == missing:
                                actor(row, sid)["supply"]["phase"] = "WAITING"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_ownership_home_scope_and_balance_violations_fail(self):
        for change in ("ownership", "home", "scope", "balance", "native_ai"):
            with self.subTest(change=change):
                rows = happy_samples()
                hunter = actor(rows[1], 901034)
                if change == "ownership": hunter["control_mode"] = "OBSERVE_ONLY"
                elif change == "home": hunter["home"]["z"] += 5
                elif change == "scope": hunter["position"]["map_id"] = 0
                elif change == "balance": hunter["supply"]["accounting"]["in_cargo"] = "1"
                else: actor(rows[1], 901000)["needs"] = dict(hunger=0.2)
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_a_missing_native_or_life_protocol_is_inconclusive(self):
        for missing in ("native", "guid", "life", "source"):
            rows = happy_samples()
            for row in rows:
                if missing == "native": row["state"]["agents"] = [a for a in row["state"]["agents"] if a["spawn_id"] != 901000]
                elif missing == "guid": actor(row, 901034).pop("runtime_guid")
                elif missing == "life": actor(row, 901034)["living_role"].pop("lifetime_ms")
                else:
                    actor(row, 901034)["supply"].update(carcass_spawn_id=None, carcass_life=None)
            self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE", missing)

    def test_transport_gaps_stale_rows_and_new_hunter_lives_do_not_stitch_cycles(self):
        for change in ("session", "sequence", "capture", "stale", "guid", "life", "fallback"):
            rows = happy_samples()
            for index, row in enumerate(rows):
                if change == "session": row["session_id"] = "a" if index < 70 else "b"
                if change == "guid" and index >= 70: actor(row, 901034)["runtime_guid"] = "new-creature"
                if change == "life" and index >= 70: actor(row, 901034)["living_role"]["lifetime_ms"] += 1
            if change == "sequence": rows[70]["sequence"] += 2
            if change == "capture": rows[70]["state"]["captured_at_ms"] = 1
            if change == "stale": rows[70]["state"]["stale"] = True
            if change == "fallback": actor(rows[70], 901034)["position"]["source"] = "spawn"
            self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE", change)

    def test_prey_respawn_or_carcass_incarnation_change_breaks_harvest(self):
        for change in ("respawn", "life", "guid"):
            rows = happy_samples()
            prey = actor(rows[45], TARGETS[901034])
            if change == "respawn":
                prey.update(alive=True, health=100, living_role=dict(enabled=True, lifetime_ms=BASE + 45000))
            elif change == "guid": prey["runtime_guid"] = "different-corpse"
            else: actor(rows[45], 901034)["supply"]["carcass_life"] = "2"
            self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE", change)

    def test_cargo_and_meals_cannot_be_created_by_movement_or_without_debit(self):
        rows = happy_samples()
        for row in rows[10:]:
            actor(row, 901034)["supply"]["cargo"] = 1
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")
        rows = happy_samples()
        for row in rows[10:]: actor(row, 901034)["needs"]["hunger"] = 0
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_a_depot_counter_away_from_the_actual_depot_and_teleport_fail(self):
        rows = happy_samples()
        for row in rows[89:]: move(actor(row, 901034), ACTORS[TARGETS[901034]]["home"])
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")
        rows = happy_samples()
        move(actor(rows[60], 901034), DEPOTS[1])
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_carcass_portions_must_be_debited_and_cannot_increase_for_same_life(self):
        for change in ("not_debited", "increased"):
            rows = happy_samples()
            for row in rows[48:]: actor(row, 901034)["supply"]["carcass_remaining"] = 6
            if change == "increased": actor(rows[47], 901034)["supply"]["carcass_remaining"] = 3
            self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_cycle_progress_from_different_recording_epochs_does_not_combine(self):
        first, second = happy_samples(), happy_samples()
        for row in first:
            row["session_id"] = "a"
            for sid in (901061, 901062):
                if actor(row, sid)["supply"]["phase"] == "HUNTING": actor(row, sid)["supply"]["phase"] = "WAITING"
        for row in second:
            row["session_id"] = "b"
            row["elapsed_seconds"] += 200
            row["state"]["captured_at_ms"] += 200000
            if actor(row, 901034)["supply"]["phase"] == "HUNTING": actor(row, 901034)["supply"]["phase"] = "WAITING"
        report = acceptance.evaluate_samples(first + second)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertEqual(len(report["cycles"]), 3)
        self.assertEqual(report["delivered_hunters"], [901061, 901062])

    def test_shared_depot_delivery_can_outweigh_a_simultaneous_real_meal(self):
        rows = happy_samples()
        for row in rows:
            t = row["elapsed_seconds"]
            gnoll = actor(row, 901061)
            supply = gnoll["supply"]
            supply.update(cargo=6 if 48 <= t < 88 else 0,
                          delivered="6" if t >= 88 else "0", consumed="1" if t >= 94 else "0")
            if 49 <= t < 83:
                move(gnoll, interpolate(ACTORS[TARGETS[901061]]["home"], DEPOTS[2], (t - 48) / 35), True)
            elif 83 <= t < 88:
                move(gnoll, DEPOTS[2]); supply["phase"] = "DELIVERING"
                gnoll.update(action="AMBIENT", action_source_goal="LOCAL_ACTIVITY")
                gnoll["living_role"].update(phase="ACTING", activity="WORK", started_at_ms=BASE + 83000)
            elif 89 <= t < 94:
                move(gnoll, DEPOTS[2]); supply["phase"] = "EATING_DEPOT"
                gnoll.update(action="AMBIENT", action_source_goal="LOCAL_ACTIVITY")
                gnoll["living_role"].update(phase="ACTING", activity="EAT", started_at_ms=BASE + 89000)
            elif t >= 88:
                move(gnoll, DEPOTS[2]); supply["phase"] = "WAITING"
                gnoll.update(action=None, action_source_goal=None)
                gnoll["living_role"].update(phase="IDLE", activity="NONE", started_at_ms=0)
                if t >= 94: gnoll["needs"]["hunger"] = 0
        rebalance(rows)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        meal = next(meal for meal in report["meals"] if meal["hunter_spawn_id"] == 901061)
        self.assertGreater(meal["stock_after"], meal["stock_before"])

    def test_ledger_counter_reset_invalidates_pending_continuity_without_food_duplication(self):
        rows = happy_samples()
        for row in rows[95:]:
            for observed in row["state"]["agents"]:
                if observed["supply"] is None:
                    continue
                supply = observed["supply"]
                supply.update(harvested="0", delivered="0", consumed="0", lost="0", cargo=0,
                              carcass_remaining=None, carcass_spawn_id=None, carcass_life=None, phase="WAITING")
                supply["accounting"] = dict(initial_food="0", harvested="0", in_depots="0", in_cargo="0", consumed="0", lost="0", conserved=True)
                if supply["depot"] is not None: supply["depot"].update(stock=0, delivered="0", consumed="0")
                observed["living_role"]["lifetime_ms"] = BASE + 94000
                observed["needs"]["hunger"] = 0
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertFalse(report["violations"])
        self.assertEqual(report["quality"]["ledger_restart_or_reset"], 1)

    def test_an_actor_counter_reset_interrupts_its_cycle_without_inventing_negative_food(self):
        rows = happy_samples()
        for row in rows[70:]: actor(row, 901034)["supply"]["harvested"] = "0"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertFalse(report["violations"])
        self.assertEqual(report["agents"]["901034"]["interruptions"]["hunter_counters_reset"], 1)

    def test_short_static_recording_is_inconclusive_but_full_window_fails(self):
        rows = happy_samples()[:1]
        repeated = []
        for index in range(4):
            row = deepcopy(rows[0]); row.update(sequence=index + 1, elapsed_seconds=index)
            row["state"]["captured_at_ms"] = BASE + index * 1000
            repeated.append(row)
        self.assertEqual(acceptance.evaluate_samples(repeated)["status"], "INCONCLUSIVE")
        self.assertEqual(acceptance.evaluate_samples(repeated, minimum_seconds=2)["status"], "FAIL")

    def test_jsonl_gzip_cli_replays_real_recorder_shape_and_preserves_inputs(self):
        rows = happy_samples()
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3]) as temp:
            directory = Path(temp)
            source, output = directory / "part-0001.jsonl.gz", directory / "supply-report.json"
            with gzip.open(source, "wt", encoding="utf-8") as stream:
                for row in rows: stream.write(json.dumps(row) + "\n")
            (directory / "summary.json").write_text(json.dumps(dict(parts=[source.name])), encoding="utf-8")
            before = source.read_bytes()
            with redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([str(directory), "--output", str(output)]), 0)
            self.assertEqual(json.loads(output.read_text())["status"], "PASS")
            self.assertEqual(source.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
