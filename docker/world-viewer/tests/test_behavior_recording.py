"""Acceptance checks exercise behavior, gaps and complete recording archives."""
import contextlib
import copy
import gzip
import io
import json
from pathlib import Path
import tempfile
import unittest

from app import behavior


POLICY = behavior.Policy(minimum_seconds=60, return_seconds=30, motion_seconds=20,
                         outside_seconds=30, stock_hunger_seconds=30, empty_stock_seconds=30, predator_hunger_seconds=60)
METADATA = {"format_version": 1, "session_id": "test-session", "interval_seconds": 5, "label": "test"}


def agent(aid=80418, role="PREDATOR"):
    return {"agent_id": aid, "spawn_id": aid, "name": "Forest Spider" if role == "PREDATOR" else role,
            "control_mode": "AI_WORLD_CONTROLLED", "alive": True, "in_combat": False,
            "position": {"source": "live", "map_id": 0, "x": -9606.48, "y": 218.8026, "z": 48.39812},
            "needs": {"hunger": 0.4}, "economy": {"food": 4, "resource": 20},
            "living_role": {"enabled": True, "extensions_enabled": True, "role": role, "status": "READY",
                            "phase": "IDLE", "activity": "NONE", "movement_purpose": "NONE", "hunt_end": "HUNT_LEASH"},
            "movement": {"moving": False, "blocked": False, "evading": False, "home_distance": 0}}


def samples(count=25, npc=None):
    return [{"kind": "sample", "sequence": i + 1, "elapsed_seconds": i * 5,
             "recorded_at_utc": f"test+{i * 5}s", "status": "fresh",
             "state": {"version": 3, "configured": True, "stale": False, "age_ms": 50,
                       "captured_at_ms": 1000000 + i * 5000, "agents": [copy.deepcopy(npc or agent())]}}
            for i in range(count)]


def summary(rows):
    return {**METADATA, "kind": "summary", "status": "finished", "stop_reason": "duration",
            "elapsed_seconds": rows[-1]["elapsed_seconds"] + 5, "samples": len(rows),
            "counts": dict(behavior.Counter(row["status"] for row in rows))}


def evaluate(rows):
    evaluator = behavior.Evaluator(METADATA, POLICY)
    for row in rows:
        evaluator.observe(row)
    return evaluator.finish(summary(rows))


def stranded():
    npc = agent()
    npc["movement"]["home_distance"] = 47.1
    npc["living_role"]["movement_purpose"] = "RETURN_STEP_BLOCKED"
    return npc


class BehaviorTests(unittest.TestCase):
    def test_version_four_preserves_return_diagnostics_through_idle(self):
        rows = samples(npc=stranded())
        for row in rows:
            row['state']['version'] = 4
            role = row['state']['agents'][0]['living_role']
            role['movement_purpose'] = 'NONE'
            role['phase'] = 'IDLE'
            role['return_recovery'] = {'failures': 8, 'strategy': 'TRAIL', 'failure': 'RETURN_NO_PATH',
                                       'rejected': {'path': 8}, 'stalled_ms': 600000}
        report = evaluate(rows)
        self.assertEqual(report['quality']['status'], 'PASS')
        self.assertEqual(report['findings'][0]['return_recovery']['strategy'], 'TRAIL')
        self.assertEqual(report['findings'][0]['check'], 'return')

    def test_empty_worker_stock_is_checked_even_without_food_to_consume(self):
        npc = agent(80683, 'WORKER')
        npc.update(entry=250, home={'map_id': 0}, work={'map_id': 0})
        npc['needs']['hunger'] = 1
        npc['economy']['food'] = 0
        rows = samples(npc=npc)
        report = evaluate(rows)
        self.assertEqual([f['check'] for f in report['findings']], ['empty_stock'])
        # Completed resupply interrupts starvation; ordinary short work/meal
        # cycles are not failures, nor is a woodworker's empty food inventory.
        for row in rows:
            if row['sequence'] % 5 == 0:
                row['state']['agents'][0]['economy']['food'] = 2
        self.assertFalse(evaluate(rows)['findings'])
        npc['entry'] = 1975
        self.assertFalse(evaluate(samples(npc=npc))['findings'])

    def test_legitimate_stationary_roles_and_stock_cap_do_not_fail(self):
        for role in ("SERVICE", "WORKER", "PREDATOR", "PREY", "GUARD"):
            with self.subTest(role=role):
                report = evaluate(samples(npc=agent(role=role)))
                self.assertEqual(report["status"], "PASS")
                self.assertFalse(report["findings"])
                self.assertEqual(report["roles"][role]["resource_increase"], 0)

    def test_long_blocked_return_survives_recovery_animation_and_idle_labels(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            role = row["state"]["agents"][0]["living_role"]
            role["phase"] = "ACTING" if i % 2 else "IDLE"
            role["movement_purpose"] = "RETURN_LOS_BLOCKED" if i % 3 == 0 else "NONE"
        report = evaluate(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["exit_code"], 3)
        finding = report["findings"][0]
        self.assertEqual((finding["spawn_id"], finding["check"], finding["duration_seconds"]), (80418, "return", 120))

    def test_actual_return_progress_resets_stationary_episode(self):
        rows = samples(npc=stranded())
        for i, row in enumerate(rows):
            row["state"]["agents"][0]["position"]["x"] += i * 2
        self.assertEqual(evaluate(rows)["status"], "PASS")

    def test_claimed_movement_without_position_change_is_detected(self):
        npc = agent()
        npc["movement"]["moving"] = True
        npc["living_role"].update(phase="MOVING", movement_purpose="FORAGE_SEARCH")
        report = evaluate(samples(npc=npc))
        self.assertEqual(report["findings"][0]["check"], "motion")
        npc["movement"]["blocked"] = True  # a root is not failed navigation
        self.assertFalse(evaluate(samples(npc=npc))["findings"])

    def test_no_episode_crosses_loss_of_fresh_live_identity(self):
        for interruption in ("stale", "absent", "dead", "abstract", "combat", "root", "clock", "sequence", "invalid"):
            with self.subTest(interruption=interruption):
                rows = samples(13, stranded())
                row = rows[6]
                npc = row["state"]["agents"][0]
                if interruption == "stale":
                    row["status"] = "stale"
                    row["state"]["stale"] = True
                elif interruption == "absent":
                    row["state"]["agents"] = [agent(42)]
                elif interruption == "dead": npc["alive"] = False
                elif interruption == "abstract": npc["position"]["source"] = "spawn"
                elif interruption == "combat": npc["in_combat"] = True
                elif interruption == "root": npc["movement"]["blocked"] = True
                elif interruption == "clock": row["state"]["captured_at_ms"] = 1
                elif interruption == "sequence": row["sequence"] += 1
                elif interruption == "invalid": npc["position"]["z"] = float("nan")
                self.assertFalse(evaluate(rows)["findings"])

    def test_clock_repeat_long_gap_duplicates_and_unknown_protocol_are_inconclusive(self):
        for problem in ("repeat", "gap", "duplicate", "protocol", "unknown-fields", "bad-position", "bad-state"):
            with self.subTest(problem=problem):
                rows = samples()
                if problem == "repeat": rows[6]["state"]["captured_at_ms"] = rows[5]["state"]["captured_at_ms"]
                elif problem == "gap": rows[6]["elapsed_seconds"] += 40
                elif problem == "duplicate": rows[6]["state"]["agents"] *= 2
                elif problem == "protocol": rows[6]["state"]["version"] = 100
                elif problem == "bad-position": rows[6]["state"]["agents"][0]["position"] = "invalid"
                elif problem == "bad-state": rows[6]["state"] = [1]
                else: del rows[6]["state"]["agents"][0]["movement"]["blocked"]
                self.assertEqual(evaluate(rows)["status"], "INCONCLUSIVE")

    def test_short_and_unobserved_runs_never_pass(self):
        self.assertEqual(evaluate(samples(3))["status"], "INCONCLUSIVE")
        for mode in ("OBSERVE_ONLY", "AI_WORLD_CONTROLLED"):
            npc = agent()
            npc["control_mode"] = mode
            npc["living_role"]["status"] = "OUTSIDE_ELWYNN"
            self.assertEqual(evaluate(samples(npc=npc))["status"], "INCONCLUSIVE")

    def test_lowering_minimum_does_not_claim_unfinished_long_checks_passed(self):
        evaluator = behavior.Evaluator(METADATA, behavior.Policy(minimum_seconds=60))
        rows = samples()
        for row in rows:
            evaluator.observe(row)
        report = evaluator.finish(summary(rows))
        self.assertEqual(report["quality"]["status"], "PASS")
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertEqual(next(c for c in report["checks"] if c["id"] == "predator_hunger")["status"], "INCONCLUSIVE")

    def test_leaving_scope_requires_observed_prior_control(self):
        rows = samples(npc=agent(54003, "TRAVELER"))
        for row in rows[1:]:
            row["state"]["agents"][0]["living_role"]["status"] = "OUTSIDE_ELWYNN"
        report = evaluate(rows)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["findings"][0]["spawn_id"], 54003)
        self.assertEqual(report["findings"][0]["check"], "outside")

    def test_hunger_with_stock_and_actual_eating(self):
        npc = agent(80683, "WORKER")
        npc["needs"]["hunger"] = 1
        self.assertEqual(evaluate(samples(npc=npc))["findings"][0]["check"], "stock_hunger")
        rows = samples(npc=npc)
        for i, row in enumerate(rows):
            if i >= 3:
                row["state"]["agents"][0]["needs"]["hunger"] = 0.1
                row["state"]["agents"][0]["economy"]["food"] = 3
        report = evaluate(rows)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["roles"]["WORKER"]["food_decrease"], 1)
        self.assertEqual(report["roles"]["WORKER"]["hunger_drops"], 1)
        npc["economy"]["food"] = 0
        self.assertFalse(evaluate(samples(npc=npc))["findings"])

    def test_hungry_predators_are_warnings_and_retained_hunt_end_is_not_a_new_event(self):
        npc = agent()
        npc["needs"]["hunger"] = 1
        report = evaluate(samples(npc=npc))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["findings"][0]["severity"], "warning")
        self.assertEqual(report["roles"]["PREDATOR"]["hunting_starts"], 0)

    def test_metrics_do_not_bridge_death_or_missing_agent(self):
        rows = samples()
        rows[2]["state"]["agents"][0]["alive"] = False
        for row in rows[3:]:
            row["state"]["agents"][0]["economy"]["food"] = 1
            row["state"]["agents"][0]["needs"]["hunger"] = 0.1
        metrics = evaluate(rows)["roles"]["PREDATOR"]
        self.assertEqual(metrics["food_decrease"], 0)
        self.assertEqual(metrics["hunger_drops"], 0)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name) / "session"
        self.directory.mkdir()

    def archive(self, rows, split=10):
        result = summary(rows)
        content = rows + [{k: v for k, v in result.items() if k not in METADATA}]
        parts = []
        for start in range(0, len(content), split):
            part = len(parts) + 1
            name = f"part-{part:04d}.jsonl.gz"
            with gzip.open(self.directory / name, "wt", encoding="utf-8") as stream:
                stream.write(json.dumps({**METADATA, "kind": "session", "part": part}) + "\n")
                for row in content[start:start + split]:
                    stream.write(json.dumps(row) + "\n")
            parts.append(name)
        result["parts"] = parts
        (self.directory / "summary.json").write_text(json.dumps(result), encoding="utf-8")
        return result

    def test_offline_archive_and_stream_evaluation_agree_and_reports_are_written(self):
        rows = samples(npc=stranded())
        self.archive(rows)
        report = behavior.analyze(self.directory, POLICY)
        self.assertEqual(report, evaluate(rows))
        output = Path(self.temp.name) / "report"
        behavior.write_report(report, output)
        self.assertEqual(json.loads((output / "behavior-report.json").read_text(encoding="utf-8"))["status"], "FAIL")
        self.assertIn("80418", (output / "behavior-report.md").read_text(encoding="utf-8"))

    def test_missing_corrupt_parts_and_traversal_cannot_pass(self):
        for problem in ("missing", "truncated", "crc", "path", "counts", "session", "running"):
            with self.subTest(problem=problem):
                info = self.archive(samples())
                part = self.directory / info["parts"][1]
                if problem == "missing": part.unlink()
                elif problem == "truncated": part.write_bytes(part.read_bytes()[:-5])
                elif problem == "crc":
                    data = bytearray(part.read_bytes())
                    data[-8] ^= 1
                    part.write_bytes(data)
                elif problem == "path": info["parts"][1] = "../secret.jsonl.gz"
                elif problem == "counts": info["samples"] += 1
                elif problem == "session": info["session_id"] = "wrong"
                elif problem == "running": info["status"] = "running"
                (self.directory / "summary.json").write_text(json.dumps(info), encoding="utf-8")
                report = behavior.analyze(self.directory, POLICY)
                self.assertEqual(report["status"], "INCONCLUSIVE")
                self.assertTrue(report["quality"]["integrity_errors"])

    def test_cli_exit_codes_and_separate_output_preserve_inputs(self):
        service = agent(role="SERVICE")
        service["economy"]["food"] = 0
        for rows, expected in ((samples(npc=service), 0), (samples(3), 2), (samples(70, stranded()), 3)):
            with self.subTest(expected=expected):
                self.archive(rows, 100)
                original = {p: p.read_bytes() for p in self.directory.iterdir()}
                with contextlib.redirect_stdout(io.StringIO()):
                    code = behavior.main([str(self.directory), "--minimum-minutes", "1",
                                          "--output", str(Path(self.temp.name) / "out")])
                self.assertEqual(code, expected)
                self.assertTrue(all(p.read_bytes() == data for p, data in original.items()))


if __name__ == "__main__":
    unittest.main()
