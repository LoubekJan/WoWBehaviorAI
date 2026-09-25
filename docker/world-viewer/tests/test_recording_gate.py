"""A deploy may start a new run only after new live telemetry is advancing."""
import contextlib
from datetime import datetime, timezone
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from app import recording_gate as gate


def snapshot(capture=1000):
    return {"version": 3, "configured": True, "stale": False, "captured_at_ms": capture,
            "received_at_ms": capture, "age_ms": 50,
            "agents": [{"control_mode": "AI_WORLD_CONTROLLED", "alive": True,
                        "position": {"source": "live", "map_id": 0},
                        "living_role": {"enabled": True, "status": "ACTIVE"}}]}


class TelemetryGateTests(unittest.TestCase):
    def wait(self, responses, after=1000, timeout=5):
        clock = [0.0]

        def sleep(seconds):
            clock[0] += seconds

        with (patch.object(gate, "fetch_state", side_effect=responses) as fetch,
              patch.object(gate.time, "monotonic", side_effect=lambda: clock[0]),
              patch.object(gate.time, "sleep", side_effect=sleep)):
            result = gate.wait_for_telemetry("http://localhost/api/state", after, timeout)
            return result, fetch.call_count

    def test_old_or_repeated_capture_does_not_prove_server_restart(self):
        result, count = self.wait([snapshot(900), snapshot(1000), snapshot(1000), snapshot(1100)])
        self.assertEqual(count, 4)
        self.assertEqual(result["captured_at_ms"], 1100)

    def test_bad_snapshots_or_outage_break_the_ready_pair(self):
        for invalid in ("stale", "empty", "observe", "dead", "spawn", "outside", "disabled", "v1", "old-age", "error"):
            with self.subTest(invalid=invalid):
                bad = snapshot(1200)
                agent = bad["agents"][0]
                if invalid == "stale": bad["stale"] = True
                elif invalid == "empty": bad["agents"] = []
                elif invalid == "observe": agent["control_mode"] = "OBSERVE_ONLY"
                elif invalid == "dead": agent["alive"] = False
                elif invalid == "spawn": agent["position"]["source"] = "spawn"
                elif invalid == "outside": agent["living_role"]["status"] = "OUTSIDE_ELWYNN"
                elif invalid == "disabled": agent["living_role"]["enabled"] = False
                elif invalid == "v1": bad["version"] = 1
                elif invalid == "old-age": bad["age_ms"] = 5001
                elif invalid == "error": bad = OSError("private connection details")
                result, count = self.wait([snapshot(1100), bad, snapshot(1300), snapshot(1400)])
                self.assertEqual((count, result["status"]), (4, "ready"))

    def test_repeated_cache_or_outage_times_out_instead_of_starting(self):
        for responses in ([snapshot()] * 3, [OSError("private")] * 3):
            with self.assertRaises(RuntimeError) as error:
                self.wait(responses, timeout=3)
            self.assertNotIn("private", str(error.exception))

    def test_cli_propagates_readiness_failure(self):
        with (patch.object(gate, "wait_for_telemetry", side_effect=RuntimeError("not ready")),
              contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(gate.main(["telemetry", "--after-ms", "1000"]), 1)


class StartedGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.now = datetime.now(timezone.utc).timestamp() * 1000
        self.after = int(self.now - 10000)
        self.data = {"session_id": "new", "build_label": "abc123", "status": "running", "last_status": "fresh",
                     "counts": {"fresh": 1}, "started_at_utc": self.date(self.now - 1000),
                     "updated_at_utc": self.date(self.now)}

    @staticmethod
    def date(ms):
        return datetime.fromtimestamp(ms / 1000, timezone.utc).isoformat()

    def save(self, data, name="new"):
        path = self.directory / ("aiworld-" + name)
        path.mkdir(exist_ok=True)
        (path / "summary.json").write_text(json.dumps(data), encoding="utf-8")

    def test_start_requires_new_running_session_of_the_exact_build(self):
        for change in ({"build_label": "other"}, {"status": "finished"}, {"last_status": "stale"},
                       {"counts": {"fresh": 0}}, {"started_at_utc": self.date(self.after - 1000)},
                       {"updated_at_utc": self.date(self.now - 100000)},
                       {"started_at_utc": self.date(self.now + 1000)}):
            with self.subTest(change=change):
                self.save({**self.data, **change})
                self.assertIsNone(gate.started_session(self.directory, "abc123", self.after, self.now))
        self.save(self.data)
        result = gate.started_session(self.directory, "abc123", self.after, self.now)
        self.assertEqual(result["session_id"], "new")
        self.assertEqual(result["fresh_samples"], 1)

    def test_past_run_of_same_commit_cannot_mask_failed_start(self):
        self.save({**self.data, "started_at_utc": self.date(self.after - 5000)}, "old")
        self.save({**self.data, "last_status": "error", "counts": {"error": 4}}, "new")
        self.assertIsNone(gate.started_session(self.directory, "abc123", self.after, self.now))
        self.save(self.data)
        self.assertEqual(gate.started_session(self.directory, "abc123", self.after, self.now)["session_id"], "new")

    def test_cli_reports_the_new_session(self):
        self.save(self.data)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = gate.main(["started", "--after-ms", str(self.after), "--build-label", "abc123",
                                "--directory", str(self.directory), "--timeout", "1"])
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output.getvalue())["session_id"], "new")

    def test_missing_build_label_is_an_argument_error(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            gate.main(["started", "--after-ms", str(self.after)])
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
