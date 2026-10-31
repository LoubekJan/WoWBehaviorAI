"""Synthetic regression fixtures are verifier tests, never live caravan proof."""
from contextlib import redirect_stdout
from copy import deepcopy
from functools import lru_cache
import gzip
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import caravan_acceptance as gate, full_population as full
from tools.realm_lab.tests.test_full_telemetry import frame

BASE = 1_000_000
PROCESS = dict(status="capture_finished", process_unchanged=True,
    actual_running_binary=dict(pid=1, start_ticks="123456", device=64512, inode=98765,
                               sha256="a"*64, version="SYNTHETIC verifier fixture; not a real world process"))


def actor(record, identity):
    return next(value for value in record["state"]["agents"] if value["spawn_id"] == identity)


def own(value, t, *, moving, destination=None):
    value.update(action="MOVE_TO" if moving else "AMBIENT", action_source_goal="LOCAL_ACTIVITY",
                 goal="LOCAL_ACTIVITY", goal_owner="INDIVIDUAL", action_started_at_ms=BASE+t*1000)
    value["living_role"].update(phase="MOVING" if moving else "ACTING", activity="NONE" if moving else "WORK",
                                started_at_ms=BASE+t*1000)
    if moving: value["destination"] = dict(map_id=725, **destination)


@lru_cache(maxsize=1)
def samples():
    """Four counter-balanced, explicitly synthetic escorted round trips."""
    specs = gate.depot_specs()
    city = specs[7]["position"]
    timeline, offset = [], 1
    for identity, source in ((1, 4), (2, 6), (1, 5), (2, 1)):
        target = specs[source]["position"]
        travel = math.ceil(gate.distance(gate.point(city), gate.point(target))/6)
        outbound, load = offset+2, offset+2+travel
        inbound, unload = load+6, load+6+travel
        timeline.append(dict(id=identity, source=source, start=offset, outbound=outbound,
                             load=load, inbound=inbound, unload=unload, end=unload+5))
        offset = unload+8
    base = frame()
    for value in base["agents"]:
        value.update(position=dict(city, source="live"), health=100, alive=True, in_combat=False,
                     runtime_guid=f"SYNTHETIC-{value['spawn_id']}", action=None, goal=None,
                     action_source_goal=None, goal_owner="", action_started_at_ms=None,
                     destination=None, movement=dict(moving=False, run_speed=7, home_distance=0))
        value["living_role"] = dict(value.get("living_role") or {}, enabled=True, extensions_enabled=True,
            lifetime_ms=BASE-1000, phase="IDLE", activity="NONE", started_at_ms=0, movement_purpose="NONE")
        if value["spawn_id"] in gate.FOOD_ACTORS:
            if not isinstance(value.get("supply"), dict):
                value["supply"] = dict(enabled=True, role="PREDATOR", depot_id=0, capacity=0)
            value["supply"].update(**{key: "0" for key in gate.METRICS if key != "loaded"}, cargo=0)
    producers = {1: 901034, 4: 901016, 5: 901022, 6: 901028}
    for source, identity in producers.items():
        value = next(value for value in base["agents"] if value["spawn_id"] == identity)
        value["supply"].update(harvested="10", delivered="10")
    result = []
    for t in range(offset+1):
        state = deepcopy(base)
        state.update(captured_at_ms=BASE+t*1000, configured=True, stale=False, age_ms=0)
        depots = {key: dict(id=key, name=spec.get("name", f"SYNTHETIC depot {key}"),
            position=deepcopy(spec["position"]), capacity=120 if key == 7 else 30,
            low_threshold=12 if key == 7 else 4, stock=10 if key in gate.SOURCES else 0,
            delivered="10" if key in gate.SOURCES else "0", consumed="0", stolen="0", exported="0")
            for key, spec in specs.items()}
        by_id = {value["spawn_id"]: value for value in state["agents"]}
        carried, city_deliveries = 0, 0
        for pair_id, pair in gate.PAIRS.items():
            trips = [trip for trip in timeline if trip["id"] == pair_id]
            current = max((trip for trip in trips if trip["start"] <= t), key=lambda x: x["start"], default=trips[0])
            source, completed = current["source"], sum(t >= trip["end"] for trip in trips)
            loaded = sum(6 for trip in trips if t >= trip["load"]+5)
            delivered = completed*6
            cargo = loaded-delivered
            carried += cargo; city_deliveries += delivered
            for trip in trips:
                if t >= trip["load"]+5:
                    depots[trip["source"]]["stock"] -= 6
                    depots[trip["source"]]["exported"] = "6"
            carrier, escort = by_id[pair["carrier_spawn_id"]], by_id[pair["escort_spawn_id"]]
            for value in (carrier, escort):
                carrying = value is carrier
                stage = "RALLY" if t < current["outbound"] else "TO_VILLAGE" if t < current["load"] else \
                    "LOADING" if t < current["load"]+5 else "TO_CITY" if t < current["unload"] else \
                    "UNLOADING" if t < current["end"] else "REST"
                value["supply"].update(cargo=cargo if carrying else 0, delivered=str(delivered) if carrying else "0")
                value["supply"]["caravan"].update(source_depot_id=source, stage=stage,
                    trip=str(trips.index(current)+1), waypoint=1 if stage in ("TO_VILLAGE", "LOADING") else 0,
                    member_ready=stage in ("RALLY", "LOADING", "UNLOADING", "REST"),
                    loaded=str(loaded) if carrying else "0", completed_trips=str(completed))
                start, end = current["outbound"], current["load"]
                target = specs[source]["position"]
                if start <= t < end:
                    ratio = (t-start)/(end-start)
                    value["position"].update({axis: city[axis]+(target[axis]-city[axis])*ratio for axis in ("x", "y", "z")})
                    value["movement"]["moving"] = True
                    own(value, start, moving=True, destination={axis: target[axis] for axis in ("x", "y", "z")})
                    value["living_role"]["movement_purpose"] = "CARAVAN_VILLAGE"
                elif current["load"] <= t < current["inbound"]:
                    value["position"].update(target)
                    if carrying and t < current["load"]+5: own(value, current["load"], moving=False)
                elif current["inbound"] <= t < current["unload"]:
                    ratio = (t-current["inbound"])/(current["unload"]-current["inbound"])
                    value["position"].update({axis: target[axis]+(city[axis]-target[axis])*ratio for axis in ("x", "y", "z")})
                    value["movement"]["moving"] = True
                    own(value, current["inbound"], moving=True, destination={axis: city[axis] for axis in ("x", "y", "z")})
                    value["living_role"]["movement_purpose"] = "CARAVAN_CITY"
                elif current["unload"] <= t < current["end"] and carrying:
                    own(value, current["unload"], moving=False)
        depots[7].update(stock=city_deliveries, delivered=str(city_deliveries))
        accounting = dict(initial_food="0", harvested="40", in_depots=str(40-carried),
                          in_cargo=str(carried), consumed="0", lost="0", conserved=True)
        for value in state["agents"]:
            supplied = value.get("supply")
            if isinstance(supplied, dict):
                supplied["accounting"] = deepcopy(accounting)
                if supplied.get("depot_id"):
                    supplied["depot"] = deepcopy(depots[supplied["depot_id"]])
                caravan = supplied.get("caravan")
                if caravan:
                    caravan["source_depot"] = deepcopy(depots[caravan["source_depot_id"]])
                    caravan["destination_depot"] = deepcopy(depots[7])
            raid = value.get("raid")
            if isinstance(raid, dict):
                raid["source_depot"] = deepcopy(depots[raid["source_depot_id"]])
                raid["camp_depot"] = deepcopy(depots[3])
        result.append(dict(status="fresh", sequence=t+1, elapsed_seconds=t, session_id="SYNTHETIC-session", state=state))
    return result


def evaluate(rows, **kwargs):
    return gate.evaluate_samples(rows, minimum_seconds=300, process_proof=PROCESS, **kwargs)


class CaravanAcceptanceTests(unittest.TestCase):
    @staticmethod
    def departing_members_after_load(members=(901042,)):
        rows = deepcopy(samples())
        source = gate.depot_specs()[5]["position"]
        city = gate.depot_specs()[7]["position"]
        for index in range(1, len(rows)):
            current, previous = actor(rows[index], 901040), actor(rows[index-1], 901040)
            if (current["supply"]["caravan"]["source_depot_id"] != 5 or
                int(current["supply"]["caravan"]["loaded"]) == int(previous["supply"]["caravan"]["loaded"])):
                continue
            row = rows[index]
            t = row["elapsed_seconds"]
            row["elapsed_seconds"] += .5; row["state"]["captured_at_ms"] += 500
            direction = gate.distance(gate.point(source), gate.point(city))
            for identity in members:
                value = actor(rows[index], identity)
                value["position"].update({axis: source[axis]+3*(city[axis]-source[axis])/direction for axis in ("x", "y", "z")})
                value["movement"]["moving"] = True
                own(value, t, moving=True, destination={axis: city[axis] for axis in ("x", "y", "z")})
                value["living_role"]["movement_purpose"] = "CARAVAN_CITY"
            return rows, index
        raise AssertionError("Synthetic source5 load not found")

    def test_owned_member_departures_after_commit_keep_the_physical_load_receipt(self):
        for members in ((901040,), (901042,), (901040, 901042)):
            rows, _ = self.departing_members_after_load(members)
            report = evaluate(rows)
            with self.subTest(members=members):
                self.assertEqual(report["status"], "PASS", report)
                receipt = next(value for value in report["qualifying_receipts"] if value["source_depot_id"] == 5)
                for identity in members:
                    role = "carrier" if identity == 901040 else "escort"
                    departure = receipt["loaded"][role+"_departure"]
                    self.assertGreaterEqual(departure["movement_started_at_ms"], receipt["loaded"]["started_at_ms"]+5000)
                    self.assertEqual(report["quality"]["owned_"+role+"_departure_after_load"], 1)

    def test_early_foreign_or_unobserved_member_departure_is_not_a_load_receipt(self):
        for identity in (901040, 901042):
            for mutation in ("early", "foreign", "absent_before_commit", "new_life", "too_far"):
                rows, index = self.departing_members_after_load((identity,))
                current, previous = actor(rows[index], identity), actor(rows[index-1], identity)
                if mutation == "early":
                    current["action_started_at_ms"] -= 1000
                    current["living_role"]["started_at_ms"] -= 1000
                elif mutation == "foreign": current["action_source_goal"] = "DEFEND"
                elif mutation == "absent_before_commit": previous["position"]["x"] += 5
                elif mutation == "too_far": current["position"]["x"] += 40
                else: current["living_role"]["lifetime_ms"] += 1
                report = evaluate(rows)
                with self.subTest(identity=identity, mutation=mutation):
                    expected = "FAIL" if mutation == "too_far" or identity == 901040 and mutation == "absent_before_commit" else "INCONCLUSIVE"
                    self.assertEqual(report["status"], expected, report)
                    self.assertNotIn(5, report["witnessed_source_depot_ids"])

    def test_four_synthetic_physical_receipts_pass_with_audited_seven_depot_ledger(self):
        report = evaluate(samples())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["witnessed_source_depot_ids"], [1, 4, 5, 6])
        self.assertEqual(len(report["qualifying_receipts"]), 4)
        self.assertEqual(report["whole_record_food_ledger_status"], "PASS")
        self.assertEqual(report["complete_ledger_frames"], report["unique_frames"])

    def test_raw_frames_are_supported_but_process_proof_is_explicit(self):
        rows = [row["state"] for row in samples()]
        self.assertEqual(evaluate(rows)["status"], "PASS")
        report = gate.evaluate_samples(rows, minimum_seconds=300)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertIn("externally_verified_unchanged_actual_world_process", report["missing_evidence"])

    def test_duplicate_capture_cannot_multiply_receipts_or_rewrite_a_snapshot(self):
        rows = list(samples()); rows.insert(20, deepcopy(rows[19]))
        report = evaluate(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(len(report["qualifying_receipts"]), 4)
        rows[20] = deepcopy(rows[20]); actor(rows[20], 901040)["health"] -= 1
        self.assertEqual(evaluate(rows)["status"], "FAIL")

    def test_fresh_poll_sequences_can_repeat_the_same_world_capture(self):
        rows = list(samples()); rows.insert(20, deepcopy(rows[19]))
        rows = [dict(row, sequence=index+1) for index, row in enumerate(rows)]
        report = evaluate(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["quality"]["identical_capture_ignored"], 1)
        rows[20] = dict(rows[20], process_identity=dict(PROCESS["actual_running_binary"], start_ticks="999999"))
        self.assertEqual(evaluate(rows)["status"], "FAIL")

    def test_conflicting_depot_copies_or_global_food_totals_fail(self):
        for mutation in ("stock", "accounting", "exported"):
            rows = list(samples()); rows[10] = deepcopy(rows[10])
            value = actor(rows[10], 901040)
            if mutation == "accounting": value["supply"]["accounting"]["in_depots"] = "41"
            else: value["supply"]["caravan"]["source_depot"][mutation] = 9 if mutation == "stock" else "1"
            with self.subTest(mutation=mutation): self.assertEqual(evaluate(rows)["status"], "FAIL")

    def test_remote_or_unowned_load_and_delivery_counters_are_not_proof(self):
        for stage in ("LOADING", "UNLOADING"):
            for mutation in ("remote", "foreign_action", "short"):
                rows = deepcopy(samples())
                for row in rows:
                    value = actor(row, 901040)
                    if value["supply"]["caravan"]["stage"] != stage: continue
                    if mutation == "remote": value["position"]["x"] += 100
                    elif mutation == "foreign_action": value["action_source_goal"] = "DEFEND"
                    else:
                        value["living_role"]["started_at_ms"] = row["state"]["captured_at_ms"]
                        value["action_started_at_ms"] = row["state"]["captured_at_ms"]
                with self.subTest(stage=stage, mutation=mutation): self.assertEqual(evaluate(rows)["status"], "FAIL")

    def test_unrelated_unloaded_dead_actor_censors_only_whole_ledger_coverage(self):
        rows = list(samples()); rows[20] = deepcopy(rows[20])
        value = actor(rows[20], 901044)
        value.update(alive=None, health=None, runtime_guid=None, supply=None, living_role=None, movement=None,
                     world_state="ABSTRACT")
        value["position"]["source"] = "spawn"
        report = evaluate(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["whole_record_food_ledger_status"], "INCONCLUSIVE")
        self.assertEqual(report["quality"]["incomplete_seven_depot_actor_ledger"], 1)

    def test_caravan_cannot_debit_the_last_village_reserve(self):
        rows = deepcopy(samples())
        for row in rows:
            actor(row, 901016)["supply"].update(harvested="9", delivered="9")
            for value in row["state"]["agents"]:
                supplied = value.get("supply")
                if not supplied: continue
                supplied["accounting"]["harvested"] = "39"
                supplied["accounting"]["in_depots"] = str(int(supplied["accounting"]["in_depots"])-1)
                copies = [supplied.get("depot")]
                if supplied.get("caravan"): copies += [supplied["caravan"]["source_depot"], supplied["caravan"]["destination_depot"]]
                if value.get("raid"): copies += [value["raid"]["source_depot"], value["raid"]["camp_depot"]]
                for depot in copies:
                    if depot and depot["id"] == 4:
                        depot["stock"] -= 1; depot["delivered"] = "9"
        report = evaluate(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("caravan_consumed_village_reserve", {v["code"] for v in report["violations"]})

    def test_escort_death_or_same_guid_new_life_cannot_complete_old_trip(self):
        for mutation in ("dead", "same_guid_new_life"):
            rows = deepcopy(samples())
            for row in rows[40:]:
                value = actor(row, 901042)
                if mutation == "dead": value.update(alive=False, health=0)
                else: value["living_role"]["lifetime_ms"] = BASE+39000
            report = evaluate(rows)
            with self.subTest(mutation=mutation):
                self.assertEqual(report["status"], "INCONCLUSIVE", report)
                self.assertNotIn(4, report["witnessed_source_depot_ids"])

    def test_missing_pair_or_capture_gap_cannot_supply_a_continuous_trip(self):
        for mutation in ("missing_pair", "gap"):
            rows = list(samples())
            if mutation == "gap": rows = rows[:40]+rows[50:]
            else:
                rows[40] = deepcopy(rows[40]); rows[40]["state"]["agents"] = [a for a in rows[40]["state"]["agents"] if a["spawn_id"] != 901042]
            with self.subTest(mutation=mutation): self.assertEqual(evaluate(rows)["status"], "INCONCLUSIVE")

    def test_stationary_rising_counters_without_paired_owned_travel_do_not_pass(self):
        rows = deepcopy(samples())
        for row in rows:
            for identity in full.ESCORT_IDS:
                value = actor(row, identity)
                value.update(action=None, action_source_goal=None, goal=None, goal_owner="")
        report = evaluate(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertEqual(report["qualifying_receipts"], [])

    def test_zero_food_waiting_is_inconclusive_not_a_success(self):
        rows = deepcopy(samples()[:5])
        for index, row in enumerate(rows):
            row["state"]["captured_at_ms"] = BASE+index*1000
            for value in row["state"]["agents"]:
                supplied = value.get("supply")
                if not supplied: continue
                for key in gate.METRICS:
                    if key != "loaded": supplied[key] = "0"
                supplied["accounting"] = dict(initial_food="0", harvested="0", in_depots="0", in_cargo="0", consumed="0", lost="0", conserved=True)
                copies = [supplied.get("depot")]
                if supplied.get("caravan"): copies += [supplied["caravan"]["source_depot"], supplied["caravan"]["destination_depot"]]
                if value.get("raid"): copies += [value["raid"]["source_depot"], value["raid"]["camp_depot"]]
                for depot in copies:
                    if depot:
                        depot.update(stock=0, delivered="0", consumed="0", stolen="0", exported="0")
        report = evaluate(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE", report)
        self.assertEqual(report["receipts"], [])

    def test_actual_process_change_or_changed_pair_objective_fails(self):
        rows = list(samples()); rows[10] = deepcopy(rows[10])
        rows[10]["process_identity"] = dict(PROCESS["actual_running_binary"], start_ticks="999999")
        self.assertEqual(evaluate(rows)["status"], "FAIL")
        rows[10].pop("process_identity")
        actor(rows[10], 901042)["supply"]["caravan"]["waypoint"] += 1
        self.assertEqual(evaluate(rows)["status"], "FAIL")

    def test_cli_reads_closed_hashed_parts_and_rejects_mismatched_process_proof(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); part = root/"part-0001.jsonl.gz"
            with gzip.open(part, "wt", encoding="utf-8") as stream:
                for row in samples(): stream.write(json.dumps(row)+"\n")
            proof = dict(PROCESS, parts=[dict(name=part.name, sha256=hashlib.sha256(part.read_bytes()).hexdigest())])
            proof_path, output = root/"capture.json", root/"report.json"
            proof_path.write_text(json.dumps(proof), encoding="utf-8")
            with redirect_stdout(io.StringIO()):
                code = gate.main([str(root), "--process-proof", str(proof_path), "--output", str(output)])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(output.read_text())["status"], "PASS")
            proof["parts"][0]["sha256"] = "b"*64
            proof_path.write_text(json.dumps(proof), encoding="utf-8")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main([str(root), "--process-proof", str(proof_path)]), 2)


if __name__ == "__main__":
    unittest.main()
