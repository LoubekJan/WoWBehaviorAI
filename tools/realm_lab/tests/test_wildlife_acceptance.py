"""Observed wildlife cycles cannot be assembled from counters or other lives."""
from contextlib import redirect_stdout
from copy import deepcopy
import gzip
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools.realm_lab import wildlife_acceptance as acceptance

PREDATOR, PREY = 901081, 901077
CAPTURE_BASE = 1_000_000


def position(x, y, z):
    return {"map_id": 725, "x": x, "y": y, "z": z}


def actor(source):
    sid, home = source["spawn_id"], source["home"]
    owned = sid in acceptance.OWNED_IDS
    role = "PREDATOR" if sid in acceptance.PREDATOR_IDS else "PREY"
    return {
        "agent_id": sid, "spawn_id": sid, "entry": source["entry"], "native_creature": not owned,
        "type": role if owned else "UNCLASSIFIED", "control_mode": "AI_WORLD_CONTROLLED" if owned else "OBSERVE_ONLY",
        "world_state": "MATERIALIZED", "position": {**position(home["x"], home["y"], home["z"]), "source": "live"},
        "home": position(home["x"], home["y"], home["z"]), "runtime_guid": f"Creature-725-{sid}",
        "alive": True, "health": 100, "needs": {"hunger": 0.8} if owned else None,
        "movement": {"home_distance": 0, "moving": False},
        "living_role": {"enabled": True, "role": role, "phase": "IDLE", "activity": "NONE", "hunt_end": "NONE",
                        "movement_purpose": "NONE", "started_at_ms": 0, "lifetime_ms": sid + 1} if owned else None,
        "action": None, "action_source_goal": None, "target": None,
    }


def move(agent, x, y, z=None):
    agent["position"].update(x=x, y=y, z=agent["position"]["z"] if z is None else z)
    agent["movement"]["home_distance"] = acceptance.distance(acceptance.point(agent["position"]), acceptance.point(agent["home"]))


def happy_samples():
    manifest = json.loads(acceptance.MANIFEST.read_text(encoding="utf-8"))
    initial = [actor(source) for source in manifest["actors"]]
    result = []
    for index, t in enumerate((0, 2, 4, 6, 8, 10, 14, 16, 18), 1):
        agents = deepcopy(initial)
        by_id = {a["spawn_id"]: a for a in agents}
        for sid in acceptance.OWNED_IDS:
            a = by_id[sid]
            if t >= 2:
                move(a, a["home"]["x"] + 3, a["home"]["y"])
        predator, prey = by_id[PREDATOR], by_id[PREY]
        if t >= 4:
            move(predator, prey["position"]["x"], prey["position"]["y"], prey["position"]["z"])
            predator["living_role"].update(phase="HUNTING", started_at_ms=CAPTURE_BASE + 4000)
            predator.update(action="ATTACK", action_source_goal="PREDATOR_HUNT",
                            target={"spawn_id": PREY, "entry": 883})
        if t >= 8:
            prey.update(alive=False, health=0)
            prey["living_role"].update(lifetime_ms=0, phase="IDLE")
            predator["living_role"].update(phase="FEEDING", started_at_ms=CAPTURE_BASE + 8000,
                                           hunt_end="PREY_DIED")
            predator.update(action="EAT", action_source_goal="FEED")
        if t >= 14:
            predator["living_role"].update(phase="ACTING", hunt_end="FED", activity="REST")
            predator["needs"]["hunger"] = 0
            predator.update(action="AMBIENT", action_source_goal="LOCAL_ACTIVITY", target=None)
        if t >= 16:
            predator["living_role"].update(phase="MOVING", activity="NONE", movement_purpose="RETURN_HOME")
            predator.update(action="MOVE_TO", action_source_goal="LOCAL_ACTIVITY")
        if t >= 18:
            move(predator, predator["home"]["x"] + 10, predator["home"]["y"])
            predator["living_role"].update(phase="IDLE", movement_purpose="NONE")
            predator.update(action=None, action_source_goal=None)
        grazer = by_id[901072]
        if t == 4:
            grazer["living_role"].update(phase="ACTING", activity="GRAZE")
        if t >= 6:
            grazer["needs"]["hunger"] = 0
        if t in (4, 6):
            forager = by_id[901086]
            forager["living_role"].update(phase="MOVING", movement_purpose="FORAGE_SEARCH")
            move(forager, forager["home"]["x"] + (3 if t == 4 else 6), forager["home"]["y"])
        result.append({"kind": "sample", "sequence": index, "elapsed_seconds": t, "status": "fresh",
                       "state": {"version": 4, "configured": True, "stale": False, "age_ms": 0,
                                 "captured_at_ms": CAPTURE_BASE + t * 1000, "agents": agents}})
    return result


def each_agent(samples, sid):
    for sample in samples:
        yield next(a for a in sample["state"]["agents"] if a["spawn_id"] == sid)


class WildlifeAcceptanceTests(unittest.TestCase):
    def test_observed_full_cycle_and_twenty_real_movements_pass_with_eighty_native(self):
        report = acceptance.evaluate_samples(happy_samples())
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(len(report["cycles"]), 1)
        cycle = report["cycles"][0]
        self.assertEqual((cycle["predator_spawn_id"], cycle["prey_spawn_id"]), (PREDATOR, PREY))
        self.assertEqual(cycle["covered"], list(acceptance.PHASES))
        self.assertLessEqual(cycle["return_end_distance"], 14)
        self.assertGreater(cycle["hunger_before"], cycle["hunger_after"] + 0.1)
        self.assertTrue(all(a["movement"] for a in report["agents"].values()))
        self.assertEqual(report["forage_901086"]["travel_yards"], 3)

    def test_exported_api_states_use_the_same_evaluator(self):
        self.assertEqual(acceptance.evaluate_samples(s["state"] for s in happy_samples())["status"], "PASS")

    def test_stale_and_missing_life_protocol_cannot_pass(self):
        for field in ("stale", "runtime_guid", "lifetime_ms"):
            with self.subTest(field=field):
                rows = happy_samples()
                for row in rows:
                    if field == "stale":
                        row["state"]["stale"] = True
                    for agent in row["state"]["agents"]:
                        if agent["spawn_id"] in acceptance.OWNED_IDS:
                            if field == "runtime_guid":
                                agent.pop(field)
                            if field == "lifetime_ms":
                                agent["living_role"].pop(field)
                report = acceptance.evaluate_samples(rows)
                self.assertEqual(report["status"], "INCONCLUSIVE")
                self.assertFalse(report["cycles"])

    def test_spawn_fallback_with_fake_phase_and_counters_is_not_live_evidence(self):
        rows = happy_samples()
        for row in rows:
            for agent in row["state"]["agents"]:
                if agent["spawn_id"] in acceptance.OWNED_IDS:
                    agent["position"]["source"] = "spawn"
                    agent["living_role"]["hunt_end"] = "FED"
                    agent["living_role"]["forage"] = {"steps_started": 1000}
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertTrue(all(a["movement"] is None for a in report["agents"].values()))
        self.assertFalse(report["cycles"])

    def test_preexisting_dead_prey_and_fed_text_do_not_prove_a_kill(self):
        rows = happy_samples()
        for row in rows[2:]:
            prey = next(a for a in row["state"]["agents"] if a["spawn_id"] == PREY)
            prey.update(alive=False, health=0)
            prey["living_role"]["lifetime_ms"] = 0
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertFalse(report["cycles"])

    def test_target_death_fallback_does_not_replace_observed_alive_to_dead(self):
        rows = happy_samples()
        prey = next(a for a in rows[4]["state"]["agents"] if a["spawn_id"] == PREY)
        prey["position"]["source"] = "last_known"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertFalse(report["cycles"])

    def test_prey_death_may_reset_role_lifetime_but_not_runtime_guid(self):
        rows = happy_samples()
        for prey in list(each_agent(rows, PREY))[4:]:
            prey["runtime_guid"] = "different-creature-life"
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_predator_incarnation_changes_break_cycles(self):
        for field in ("runtime_guid", "lifetime_ms"):
            with self.subTest(field=field):
                rows = happy_samples()
                for predator in list(each_agent(rows, PREDATOR))[7:]:
                    if field == "runtime_guid":
                        predator[field] = "new-runtime-life"
                    else:
                        predator["living_role"][field] += 1
                report = acceptance.evaluate_samples(rows)
                self.assertEqual(report["status"], "INCONCLUSIVE")
                self.assertFalse(report["cycles"])

    def test_prey_respawn_with_reused_guid_cannot_stitch_feeding(self):
        rows = happy_samples()
        prey = next(a for a in rows[5]["state"]["agents"] if a["spawn_id"] == PREY)
        prey.update(alive=True, health=100)
        prey["living_role"]["lifetime_ms"] = PREY + 2
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertFalse(report["cycles"])

    def test_session_sequence_capture_and_stale_gaps_do_not_stitch_cycles(self):
        for gap in ("session", "sequence", "capture", "stale"):
            with self.subTest(gap=gap):
                rows = happy_samples()
                if gap == "session":
                    for index, row in enumerate(rows):
                        row["session_id"] = "a" if index < 7 else "b"
                elif gap == "sequence":
                    rows[7]["sequence"] += 10
                elif gap == "capture":
                    rows[7]["state"]["captured_at_ms"] = 100
                else:
                    rows[6]["state"]["stale"] = True
                report = acceptance.evaluate_samples(rows)
                self.assertEqual(report["status"], "INCONCLUSIVE")
                self.assertFalse(report["cycles"])

    def test_fed_requires_observed_feeding_and_actual_hunger_drop(self):
        for missing in ("feeding", "drop", "action"):
            with self.subTest(missing=missing):
                rows = happy_samples()
                for predator in each_agent(rows, PREDATOR):
                    if missing == "feeding" and predator["living_role"]["phase"] == "FEEDING":
                        predator["living_role"]["phase"] = "IDLE"
                    if missing == "drop":
                        predator["needs"]["hunger"] = 0.8
                    if missing == "action":
                        predator["action"] = "AMBIENT"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_feeding_near_home_does_not_prove_a_return(self):
        rows = happy_samples()
        for predator in list(each_agent(rows, PREDATOR))[6:]:
            move(predator, predator["home"]["x"] + 10, predator["home"]["y"])
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_hunger_drop_without_grazing_does_not_prove_prey_food(self):
        rows = happy_samples()
        for agent in each_agent(rows, 901072):
            agent["living_role"]["activity"] = "LOOK"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertIn("observed_prey_grazing_then_hunger_drop", report["missing_evidence"])

    def test_real_movement_is_not_assembled_across_respawns(self):
        rows = happy_samples()
        for index, agent in enumerate(each_agent(rows, 901091)):
            agent["runtime_guid"] = f"life-{index}"
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertIsNone(report["agents"]["901091"]["movement"])

    def test_native_wildlife_is_not_silently_counted_as_controlled(self):
        rows = happy_samples()
        for agent in each_agent(rows, 901091):
            agent.update(native_creature=True, control_mode="OBSERVE_ONLY")
        report = acceptance.evaluate_samples(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertIn({"code": "wildlife_not_controlled", "spawn_id": 901091}, report["violations"])

    def test_wrong_role_home_map_and_extra_owned_id_fail(self):
        for violation in ("role", "home", "map", "extra"):
            with self.subTest(violation=violation):
                rows = happy_samples()
                agent = next(a for a in rows[-1]["state"]["agents"] if a["spawn_id"] == PREDATOR)
                if violation == "role":
                    agent["type"] = "PREY"
                elif violation == "home":
                    agent["home"]["x"] += 3
                elif violation == "map":
                    agent["position"]["map_id"] = 0
                else:
                    rows[-1]["state"]["agents"][0]["control_mode"] = "AI_WORLD_CONTROLLED"
                self.assertEqual(acceptance.evaluate_samples(rows)["status"], "FAIL")

    def test_missing_owned_id_is_inconclusive(self):
        rows = happy_samples()
        for row in rows:
            row["state"]["agents"] = [a for a in row["state"]["agents"] if a["spawn_id"] != 901091]
        self.assertEqual(acceptance.evaluate_samples(rows)["status"], "INCONCLUSIVE")

    def test_full_stationary_window_is_an_observed_failure(self):
        rows = happy_samples()
        for agent in each_agent(rows, 901091):
            move(agent, agent["home"]["x"], agent["home"]["y"])
        report = acceptance.evaluate_samples(rows, minimum_seconds=10)
        self.assertEqual(report["status"], "FAIL")
        self.assertIn({"code": "no_observed_movement_during_full_window", "spawn_id": 901091}, report["violations"])

    def test_parted_gzip_cli_and_exit_codes(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3]) as temporary:
            path = Path(temporary)
            samples = happy_samples()
            for index, rows in enumerate((samples[:5], samples[5:]), 1):
                with gzip.open(path / f"part-{index:04}.jsonl.gz", "wt", encoding="utf-8") as stream:
                    for row in ({"kind": "session", "session_id": "one-recording"}, *rows):
                        stream.write(json.dumps(row) + "\n")
            report_path = path / "acceptance.json"
            with redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([str(path), "--output", str(report_path)]), 0)
            self.assertEqual(json.loads(report_path.read_text())["status"], "PASS")
            empty = path / "empty.jsonl"
            empty.write_text("", encoding="utf-8")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([str(empty)]), 2)
            bad = path / "bad.jsonl"
            rows = happy_samples()
            rows[0]["state"]["agents"][0]["control_mode"] = "AI_WORLD_CONTROLLED"
            bad.write_text("\n".join(json.dumps(row) for row in rows), encoding="utf-8")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([str(bad)]), 3)


if __name__ == "__main__":
    unittest.main()
