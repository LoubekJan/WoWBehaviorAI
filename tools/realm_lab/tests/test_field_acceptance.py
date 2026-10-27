"""Physical field proof requires owned work and reconciled finite transfers."""
from contextlib import redirect_stdout
from copy import deepcopy
import gzip
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import field_acceptance as acceptance, field_population as fields
from tools.realm_lab.tests.test_field_population import frame

BASE = 1_000_000
POPULATION = fields.Population.load()
ACTORS = {actor.spawn_id: actor for actor in POPULATION.settlement.actors}
WORKERS = {actor.spawn_id: actor for actor in POPULATION.recruits}
DEFINITIONS = {definition["id"]: definition for definition in POPULATION.fields}
DEPOTS = {depot["id"]: depot["position"] for depot in fields.raids.consumers.supply.DEPOTS}
DEPOTS[3] = fields.raids.DEPOT["position"]
DEPOTS.update({key: definition["warehouse"]["position"] for key, definition in DEFINITIONS.items()})


def xyz(value): return tuple(value[key] for key in ("x", "y", "z"))
def blend(a, b, ratio): return {key: a[key]+(b[key]-a[key])*ratio for key in ("x", "y", "z")}
def actor(record, sid): return next(actor for actor in record["state"]["agents"] if actor["spawn_id"] == sid)


def move(current, position, moving=False):
    current["position"].update(position)
    current["movement"].update(moving=moving, home_distance=math.dist(xyz(current["position"]), xyz(current["home"])))


def owned(current, phase, activity, started, *, moving=False, purpose="NONE"):
    current.update(action="MOVE_TO" if moving else "AMBIENT", action_source_goal="LOCAL_ACTIVITY",
        goal="LOCAL_ACTIVITY", goal_owner="INDIVIDUAL", action_started_at_ms=BASE+started*1000)
    current["living_role"].update(phase="MOVING" if moving else "ACTING", activity=activity,
        started_at_ms=BASE+started*1000, movement_purpose=purpose)
    current["supply"]["phase"] = phase


def happy_samples(*, end=300):
    """Synthetic test fixture; never a live recording or acceptance receipt."""
    initial = frame()
    for current in initial["agents"]:
        spec = ACTORS[current["spawn_id"]]
        current.update(entry=spec.entry, home=dict(map_id=725, x=spec.x, y=spec.y, z=spec.z), work=None,
            world_state="MATERIALIZED", health=100, alive=True, in_combat=False)
        current["position"].update(x=spec.x, y=spec.y, z=spec.z, source="live")
        if not current["native_creature"]:
            binding = next(actor for actor in POPULATION.actors if actor.spawn_id == spec.spawn_id)
            current.update(economy=dict(food=0, resource=0, money="0"),
                movement=dict(moving=False, run_speed=7.0, home_distance=0),
                type={0: "CIVILIAN", 1: "GUARD", 5: "COMBATANT", 6: "PREDATOR", 7: "PREY"}[binding.agent_type],
                world_faction=getattr(binding, "world_faction", 0))
            current["living_role"].update(lifetime_ms=BASE-1000, phase="IDLE", activity="NONE", started_at_ms=0, movement_purpose="NONE")
        supplied = current.get("supply")
        if supplied:
            supplied.update(phase="WAITING", harvested="0", delivered="0", consumed="0", lost="0", stolen="0", last_result="READY")
            depot_id = supplied["depot_id"]
            supplied["depot"] = dict(id=depot_id, name=f"depot-{depot_id}", position=deepcopy(DEPOTS[depot_id]),
                stock=0, capacity=30, low_threshold=4, delivered="0", consumed="0", stolen="0")
        if current.get("raid"):
            for key, depot_id in (("source_depot", 1), ("camp_depot", 3)):
                current["raid"][key] = dict(id=depot_id, name=f"depot-{depot_id}", position=deepcopy(DEPOTS[depot_id]),
                    stock=0, capacity=30, low_threshold=4, delivered="0", consumed="0", stolen="0")
    initial.update(configured=True, age_ms=0)
    result = []
    for t in range(end+1):
        state = deepcopy(initial)
        state["captured_at_ms"] = BASE+t*1000
        harvested, delivered, consumed = int(t >= 56), int(t >= 102), int(t >= 108)
        accounting = dict(initial_food="0", harvested=str(12*harvested), in_cargo=str(12*(harvested-delivered)),
            in_depots=str(12*(delivered-consumed)), consumed=str(12*consumed), lost="0", conserved=True)
        for current in state["agents"]:
            supplied = current.get("supply")
            if supplied:
                supplied["accounting"] = deepcopy(accounting)
                if supplied["depot_id"] >= 4:
                    supplied["depot"].update(stock=4*(delivered-consumed), delivered=str(4*delivered), consumed=str(4*consumed))
            sid = current["spawn_id"]
            if sid not in WORKERS: continue
            spec, definition = WORKERS[sid], DEFINITIONS[WORKERS[sid].field_id]
            target, depot, home = definition["work_points"][spec.worker_ordinal], DEPOTS[spec.depot_id], current["home"]
            current["needs"]["hunger"] = .8 if t < 108 else .1
            supplied.update(harvested=str(harvested), delivered=str(delivered), consumed=str(consumed),
                cargo=harvested-delivered, field_remaining=24-4*harvested)
            if 0 <= t <= 40:
                move(current, blend(home, target, t/40), t < 40)
                owned(current, "TO_FIELD", "NONE", 0, moving=True, purpose="FARM_FIELD")
            elif 41 <= t <= 55:
                move(current, target)
                owned(current, "WORKING_FIELD", "WORK", 41)
            elif t == 56:
                move(current, target)
            elif 57 <= t <= 96:
                move(current, blend(target, depot, (t-57)/39), t < 96)
                owned(current, "TO_WAREHOUSE", "NONE", 57, moving=True, purpose="FARM_DEPOT")
            elif 97 <= t <= 101:
                move(current, depot)
                owned(current, "DELIVERING", "WORK", 97)
            elif t == 102:
                move(current, depot)
            elif 103 <= t <= 107:
                move(current, depot)
                owned(current, "EATING_DEPOT", "EAT", 103)
            elif t == 108:
                move(current, depot)
            elif 109 <= t <= 128:
                move(current, blend(depot, home, (t-109)/19), t < 128)
                owned(current, "RETURNING_HOME", "NONE", 109, moving=True, purpose="FARM_HOME")
            elif 129 <= t <= 138:
                move(current, home)
                owned(current, "RESTING_HOME", "REST", 129)
            if t >= 139:
                supplied["field_workpoint_index"] = spec.worker_ordinal+4
                supplied["field_workpoint"] = {key: definition["work_points"][spec.worker_ordinal+4][key] for key in ("map_id", "x", "y", "z")}
                supplied["last_result"] = "RESTED_HOME"
        result.append(dict(sequence=t+1, elapsed_seconds=t, session_id="SYNTHETIC-field-test", synthetic_test_fixture=True, status="fresh", state=state))
    return result


def separate_trip_samples(source):
    """Synthetic full-stock schedule: production/home, maintenance, meal/home.

    Nonzero opening counter balances are deliberately unproved prehistory.
    Only new owned, timed actions in this fixture can supply positive evidence.
    """
    rows = deepcopy(source)
    for t, row in enumerate(rows):
        harvested, delivered, consumed = int(t >= 56), int(t >= 102), int(t >= 245)
        accounting = dict(initial_food="0", harvested=str(78+12*harvested), in_cargo=str(12*(harvested-delivered)),
            in_depots=str(78+12*(delivered-consumed)), consumed=str(12*consumed), lost="0", conserved=True)
        for current in row["state"]["agents"]:
            supplied = current.get("supply")
            if supplied: supplied["accounting"] = deepcopy(accounting)
            sid = current["spawn_id"]
            if sid not in WORKERS: continue
            spec, definition = WORKERS[sid], DEFINITIONS[WORKERS[sid].field_id]
            initial, second = definition["work_points"][spec.worker_ordinal], definition["work_points"][spec.worker_ordinal+4]
            index = spec.worker_ordinal + (0 if t < 133 or t >= 276 else 4 if t < 219 else 8)
            seed = 6 if spec.worker_ordinal < 2 else 7
            supplied.update(phase="WAITING", harvested=str(seed+harvested), delivered=str(seed+delivered), consumed=str(consumed),
                cargo=harvested-delivered, field_remaining=24-4*harvested, field_workpoint_index=index,
                field_workpoint={key: definition["work_points"][index][key] for key in ("map_id", "x", "y", "z")}, last_result="READY")
            supplied["depot"].update(stock=26+4*(delivered-consumed), delivered=str(26+4*delivered), consumed=str(4*consumed))
            current.update(action=None, action_source_goal=None, action_started_at_ms=None, goal=None, goal_owner="")
            current["living_role"].update(phase="IDLE", activity="NONE", started_at_ms=0, movement_purpose="NONE")
            current["needs"]["hunger"] = .05+.003*t if t < 245 else .003*(t-245)
            home, depot = current["home"], DEPOTS[spec.depot_id]
            move(current, home)
            if t <= 40:
                move(current, blend(home, initial, t/40), t < 40)
                owned(current, "TO_FIELD", "NONE", 0, moving=True, purpose="FARM_FIELD")
            elif t <= 55:
                move(current, initial); owned(current, "WORKING_FIELD", "WORK", 41)
            elif t == 56: move(current, initial)
            elif t <= 96:
                move(current, blend(initial, depot, (t-57)/39), t < 96)
                owned(current, "TO_WAREHOUSE", "NONE", 57, moving=True, purpose="FARM_DEPOT")
            elif t <= 101:
                move(current, depot); owned(current, "DELIVERING", "WORK", 97)
            elif t == 102: move(current, depot)
            elif t <= 122:
                move(current, blend(depot, home, (t-103)/19), t < 122)
                owned(current, "RETURNING_HOME", "NONE", 103, moving=True, purpose="FARM_HOME")
            elif t <= 132:
                owned(current, "RESTING_HOME", "REST", 123)
            elif t == 133: supplied["last_result"] = "RESTED_HOME"
            elif t <= 173:
                move(current, blend(home, second, (t-134)/39), t < 173)
                owned(current, "TO_FIELD", "NONE", 134, moving=True, purpose="FARM_FIELD")
            elif t <= 188:
                move(current, second); owned(current, "WORKING_FIELD", "WORK", 174)
            elif t <= 208:
                move(current, blend(second, home, (t-189)/19), t < 208)
                owned(current, "RETURNING_HOME", "NONE", 189, moving=True, purpose="FARM_HOME")
                supplied["last_result"] = "FIELD_MAINTENANCE"
            elif t <= 218: owned(current, "RESTING_HOME", "REST", 209)
            elif t == 219: supplied["last_result"] = "RESTED_HOME"
            elif t <= 239:
                move(current, blend(home, depot, (t-220)/19), t < 239)
                owned(current, "TO_WAREHOUSE", "NONE", 220, moving=True, purpose="FARM_DEPOT")
            elif t <= 244:
                move(current, depot); owned(current, "EATING_DEPOT", "EAT", 240)
            elif t == 245: move(current, depot)
            elif t <= 265:
                move(current, blend(depot, home, (t-246)/19), t < 265)
                owned(current, "RETURNING_HOME", "NONE", 246, moving=True, purpose="FARM_HOME")
            elif t <= 275: owned(current, "RESTING_HOME", "REST", 266)
            elif t == 276: supplied["last_result"] = "RESTED_HOME"
    return rows


class FieldAcceptanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.good = happy_samples()

    def test_all_twelve_chains_share_a_complete_fresh_three_hundred_second_epoch(self):
        report = acceptance.evaluate_samples(self.good)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["completed_farmers"], sorted(fields.RECRUIT_IDS))
        self.assertEqual((report["expected_owned"], report["expected_native"], len(report["chains"])), (40, 60, 12))
        self.assertEqual(report["observed_seconds"], 300)
        for chain in report["chains"]:
            production, meal = chain["production_return"], chain["meal_return"]
            self.assertEqual(production["covered"], ["TO_FIELD", "FIELD_ARRIVED", "HARVESTED", "CARGO", "DELIVERED", "PRODUCTION_HOME_REST"])
            self.assertGreaterEqual(production["field_travel_yards"], 2)
            self.assertGreaterEqual(production["warehouse_travel_yards"], 2)
            self.assertEqual(production["returned_seconds"], 139)
            self.assertEqual(meal["returned_seconds"], 139)
            for receipt in (production["home_rest_receipt"], meal["home_rest_receipt"]):
                self.assertGreaterEqual(receipt["completion_capture_ms"]-receipt["started_at_ms"], 10000)

    def test_production_home_rest_survives_full_stock_maintenance_until_later_meal_return(self):
        rows = separate_trip_samples(self.good)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["criterion"], "linked_production_return_and_later_meal_return")
        self.assertEqual(report["completed_farmers"], sorted(fields.RECRUIT_IDS))
        for chain in report["chains"]:
            production, meal = chain["production_return"], chain["meal_return"]
            self.assertEqual((production["returned_seconds"], meal["meal_seconds"], meal["returned_seconds"]), (133, 245, 276))
            self.assertEqual(production["runtime_guid"], meal["runtime_guid"])
            self.assertEqual(production["lifetime_ms"], meal["lifetime_ms"])
            self.assertEqual(production["epoch"], meal["epoch"])
        self.assertEqual(acceptance.evaluate_samples(rows[:271], minimum_seconds=100)["status"], "INCONCLUSIVE")
        for mutation in ("no_production", "no_meal_return", "stale_between_receipts", "new_life_between_receipts"):
            candidate = deepcopy(rows)
            if mutation == "no_production": candidate = candidate[120:]
            elif mutation == "no_meal_return":
                for row in candidate[266:]:
                    current = actor(row, 901016)
                    current["supply"].update(phase="WAITING", field_workpoint_index=8,
                        field_workpoint={key: DEFINITIONS[4]["work_points"][8][key] for key in ("map_id", "x", "y", "z")})
            elif mutation == "stale_between_receipts": candidate[200]["status"] = "stale"
            else:
                for row in candidate[200:]: actor(row, 901016)["living_role"]["lifetime_ms"] = BASE+200000
            result = acceptance.evaluate_samples(candidate, minimum_seconds=100)
            with self.subTest(mutation=mutation): self.assertEqual(result["status"], "INCONCLUSIVE", result)

    def test_action_source_duration_workpoint_and_free_rewards_are_real_violations(self):
        for mutation in ("work_owner", "work_duration", "work_away", "delivery_owner", "meal_owner", "foreign_field", "foreign_depot", "personal_food", "free_hunger", "crop_creation", "home", "teleport"):
            rows = deepcopy(self.good)
            current = actor(rows[55], 901016)
            if mutation == "work_owner": current["action_source_goal"] = "FEED"
            elif mutation == "work_duration":
                for row in rows[41:56]: owned(actor(row, 901016), "WORKING_FIELD", "WORK", 50)
            elif mutation == "work_away": move(current, current["home"])
            elif mutation == "delivery_owner": actor(rows[101], 901016)["goal_owner"] = "GROUP"
            elif mutation == "meal_owner": actor(rows[107], 901016)["action"] = "EAT"
            elif mutation == "foreign_field": current["supply"]["field_id"] = 5
            elif mutation == "foreign_depot": current["supply"]["depot_id"] = 5
            elif mutation == "personal_food": actor(rows[56], 901016)["economy"]["food"] = 1
            elif mutation == "free_hunger": actor(rows[56], 901016)["needs"]["hunger"] = .1
            elif mutation == "crop_creation":
                for sid in fields.FIELD_WORKERS[4]: actor(rows[56], sid)["supply"]["field_remaining"] = 24
            elif mutation == "home": current["home"]["x"] = 0
            else: move(current, dict(x=500, y=500, z=0))
            report = acceptance.evaluate_samples(rows)
            with self.subTest(mutation=mutation): self.assertEqual(report["status"], "FAIL", report)

    def test_missing_stale_duplicate_gap_and_incarnation_cannot_stitch_work(self):
        for mutation in ("stale", "gap", "missing_actor", "guid", "life", "late_start", "no_return"):
            rows = deepcopy(self.good)
            if mutation == "stale": rows[48]["status"] = "stale"
            elif mutation == "gap": rows.pop(48)
            elif mutation == "missing_actor": rows[48]["state"]["agents"].pop()
            elif mutation in ("guid", "life"):
                for row in rows[48:]:
                    current = actor(row, 901016)
                    if mutation == "guid": current["runtime_guid"] = "Creature-new"
                    else: current["living_role"]["lifetime_ms"] = BASE+48000
            elif mutation == "late_start":
                for row in rows[41:45]:
                    current = actor(row, 901016)
                    current["supply"]["phase"] = "WAITING"
                    current["supply"]["field_workpoint_index"] = 0
                    current["supply"]["field_workpoint"] = {key: DEFINITIONS[4]["work_points"][0][key] for key in ("map_id", "x", "y", "z")}
                    current["action"] = None
            else:
                for row in rows[129:]:
                    current = actor(row, 901016)
                    current["supply"]["phase"] = "WAITING"
                    current["supply"]["field_workpoint_index"] = 0
                    current["supply"]["field_workpoint"] = {key: DEFINITIONS[4]["work_points"][0][key] for key in ("map_id", "x", "y", "z")}
            report = acceptance.evaluate_samples(rows)
            with self.subTest(mutation=mutation): self.assertEqual(report["status"], "INCONCLUSIVE", report)
        direct = [row["state"] for row in self.good]
        direct.insert(48, deepcopy(direct[47]))
        report = acceptance.evaluate_samples(direct)
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["quality"]["identical_capture_ignored"], 1)

    def test_partial_observation_never_passes_and_same_capture_replay_never_adds_duration(self):
        self.assertEqual(acceptance.evaluate_samples(self.good[:150])["status"], "INCONCLUSIVE")
        self.assertEqual(acceptance.evaluate_samples(self.good[:135], minimum_seconds=100)["status"], "INCONCLUSIVE")
        frozen = [deepcopy(self.good[0]["state"]) for _ in range(350)]
        report = acceptance.evaluate_samples(frozen)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertEqual(report["observed_seconds"], 0)

    def test_simultaneous_own_warehouse_delivery_and_meal_can_increase_net_stock(self):
        rows = deepcopy(self.good)
        for t, row in enumerate(rows):
            # One Top worker is six seconds ahead. Its meal coincides with
            # three other workers delivering; net stock increases by two.
            replacement = deepcopy(actor(self.good[min(t+6, 300)], 901017))
            for container, key in ((replacement, "action_started_at_ms"), (replacement["living_role"], "started_at_ms")):
                if container.get(key): container[key] -= 6000
            row["state"]["agents"] = [replacement if current["spawn_id"] == 901017 else current for current in row["state"]["agents"]]
            farmers = [current for current in row["state"]["agents"] if current["spawn_id"] in WORKERS]
            totals = {key: sum(int(current["supply"][key]) for current in farmers) for key in ("harvested", "delivered", "consumed")}
            accounting = dict(initial_food="0", harvested=str(totals["harvested"]), consumed=str(totals["consumed"]),
                in_cargo=str(totals["harvested"]-totals["delivered"]), in_depots=str(totals["delivered"]-totals["consumed"]), lost="0", conserved=True)
            for current in row["state"]["agents"]:
                supplied = current.get("supply")
                if not supplied: continue
                supplied["accounting"] = deepcopy(accounting)
                if supplied["depot_id"] < 4: continue
                members = [farmer for farmer in farmers if farmer["supply"]["depot_id"] == supplied["depot_id"]]
                delivered = sum(int(farmer["supply"]["delivered"]) for farmer in members)
                consumed = sum(int(farmer["supply"]["consumed"]) for farmer in members)
                supplied["depot"].update(stock=delivered-consumed, delivered=str(delivered), consumed=str(consumed))
                supplied["field_remaining"] = 24-sum(int(farmer["supply"]["harvested"]) for farmer in members)
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "PASS", report)
        self.assertGreater(actor(rows[102], 901017)["supply"]["depot"]["stock"], actor(rows[101], 901017)["supply"]["depot"]["stock"])

    def test_all_six_depot_copies_capacity_and_global_balances_remain_strict(self):
        for mutation in ("accounting", "depot_copy", "old_capacity", "uncredited_stock", "cargo", "rotation"):
            rows = deepcopy(self.good)
            current = actor(rows[102], 901016)
            if mutation == "accounting": current["supply"]["accounting"]["consumed"] = "1"
            elif mutation == "depot_copy": current["supply"]["depot"]["stock"] = 5
            elif mutation == "old_capacity": actor(rows[102], 901034)["supply"]["depot"]["capacity"] = 31
            elif mutation == "uncredited_stock":
                for sid in fields.FIELD_WORKERS[4]: actor(rows[102], sid)["supply"]["depot"]["delivered"] = "5"
            elif mutation == "cargo": current["supply"]["cargo"] = 1
            else:
                current = actor(rows[100], 901016)
                current["supply"]["field_workpoint_index"] = 4
                current["supply"]["field_workpoint"] = {key: DEFINITIONS[4]["work_points"][4][key] for key in ("map_id", "x", "y", "z")}
            report = acceptance.evaluate_samples(rows)
            with self.subTest(mutation=mutation): self.assertEqual(report["status"], "FAIL", report)

    def test_cli_reads_closed_gzip_and_recorder_parts_without_modifying_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "part-0001.jsonl.gz"
            with gzip.open(path, "wt", encoding="utf-8") as stream:
                for row in self.good: stream.write(json.dumps(row)+"\n")
            before = path.read_bytes()
            (root / "summary.json").write_text(json.dumps(dict(parts=[path.name])))
            output = root / "proof.json"
            with redirect_stdout(io.StringIO()): code = acceptance.main([str(root), "--output", str(output)])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(output.read_text())["status"], "PASS")
            self.assertEqual(path.read_bytes(), before)
            bad = root / "bad.jsonl.gz"
            bad.write_bytes(before[:-8])
            with redirect_stdout(io.StringIO()): code = acceptance.main([str(bad)])
            self.assertEqual(code, 2)


if __name__ == "__main__": unittest.main()
