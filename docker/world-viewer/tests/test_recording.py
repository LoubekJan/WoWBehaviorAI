"""Exercise the recorder through real HTTP and read back every gzip part."""
from __future__ import annotations

import argparse
import contextlib
import copy
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from app import record


def state() -> dict:
    batch = json.loads((Path(__file__).parent / "fixtures" / "telemetry_v2.json").read_text(encoding="utf-8"))
    return {**batch, "configured": True, "stale": False, "age_ms": 50,
            "received_at_ms": 1_800_000_000_000}


class RecordingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.stop = threading.Event()
        self.responses = []
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                owner.requests.append((self.path, self.headers.get("Authorization")))
                if owner.responses:
                    status, body = owner.responses.pop(0)
                    if not owner.responses:
                        owner.stop.set()
                else:
                    status, body = 200, state()
                if status == 0:
                    self.wfile.write(b"broken HTTP status line\r\n\r\n")
                    self.close_connection = True
                    return
                body = json.dumps(body).encode("utf-8") if isinstance(body, dict) else body
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args: object) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.01), daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.args = argparse.Namespace(
            url=f"http://127.0.0.1:{self.server.server_port}/api/state", output=Path(self.temp.name),
            hours=2 / 3600, interval=0.01, timeout=0.5, max_part_mib=64, label="test — Elwynn",
        )

    def close_server(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def run_recording(self) -> tuple[int, dict, list[dict]]:
        with contextlib.redirect_stdout(io.StringIO()):
            code = record.record(self.args, self.stop)
        directory = next(Path(self.temp.name).iterdir())
        summary = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
        rows = []
        for part in summary["parts"]:
            with gzip.open(directory / part, "rt", encoding="utf-8") as handle:
                content = [json.loads(line) for line in handle]
            self.assertEqual(content[0]["kind"], "session")
            self.assertEqual(content[0]["session_id"], summary["session_id"])
            rows.extend(content)
        self.assertEqual(rows[-1]["kind"], "summary")
        return code, summary, rows

    def test_outage_stale_restart_and_recovery_preserve_full_states(self) -> None:
        fresh = state()
        waiting = {**fresh, "captured_at_ms": None, "received_at_ms": None,
                   "age_ms": None, "stale": True, "agents": []}
        stale = {**fresh, "stale": True, "age_ms": 35000}
        restarted = copy.deepcopy(fresh)
        restarted["captured_at_ms"] = 1  # source clock can reset; do not discard it
        restarted["agents"][0]["future_field"] = {"exact_money": "18446744073709551615"}
        self.responses = [(200, {**waiting, "configured": False}), (200, waiting), (200, fresh),
                          (200, stale), (503, b"private upstream text"), (200, b"<html>not JSON</html>"),
                          (200, restarted)]
        code, summary, rows = self.run_recording()
        samples = [row for row in rows if row["kind"] == "sample"]
        self.assertEqual(code, 0)
        self.assertEqual(summary["samples"], 7)
        self.assertEqual(summary["counts"], {"unconfigured": 1, "waiting": 1, "fresh": 2, "stale": 1, "error": 2})
        self.assertEqual(summary["stop_reason"], "stopped")
        self.assertEqual(summary["max_agents"], len(fresh["agents"]))
        self.assertEqual(samples[2]["state"], fresh)
        self.assertEqual(samples[3]["state"], stale)
        self.assertEqual(samples[-1]["state"], restarted)
        self.assertEqual(samples[4]["error"], {"type": "HTTPError", "http_status": 503})
        self.assertNotIn("private upstream", json.dumps(rows))
        self.assertNotIn("state", samples[4])
        self.assertEqual([sample["sequence"] for sample in samples], list(range(1, 8)))
        self.assertEqual(self.requests, [("/api/state", None)] * 7)

    def test_rotation_preserves_every_sample_and_seals_each_gzip(self) -> None:
        self.args.max_part_mib = 1 / (1024 * 1024)  # rotate after every complete record
        self.responses = [(200, state()) for _ in range(3)]
        code, summary, rows = self.run_recording()
        self.assertEqual(code, 0)
        self.assertEqual(len(summary["parts"]), 4)  # three samples + final summary
        self.assertEqual([r["sequence"] for r in rows if r["kind"] == "sample"], [1, 2, 3])

    def test_duration_is_automatic_and_empty_test_is_not_successful(self) -> None:
        self.args.hours = 0.08 / 3600
        self.args.interval = 5  # waiting ends at deadline, not five seconds later
        empty = {**state(), "agents": []}
        with patch.object(record, "fetch_state", return_value=empty):
            code, summary, _ = self.run_recording()
        self.assertEqual(code, 2)
        self.assertFalse(summary["usable"])
        self.assertEqual(summary["stop_reason"], "duration")
        self.assertEqual(summary["counts"], {"empty": 1})
        self.assertLess(summary["elapsed_seconds"], 1)

    def test_slow_fetch_skips_missed_slots_without_burst(self) -> None:
        calls = []
        clock = [0.0]
        self.args.interval = 0.05

        def slow_fetch(url: str, timeout: float) -> dict:
            calls.append(clock[0])
            if len(calls) == 1:
                clock[0] += 0.12
            else:
                self.stop.set()
            return state()

        def advance(seconds: float) -> None:
            clock[0] += seconds

        with (patch.object(record, "fetch_state", side_effect=slow_fetch),
              patch.object(record.time, "monotonic", side_effect=lambda: clock[0]),
              patch.object(self.stop, "wait", side_effect=advance)):
            _, summary, _ = self.run_recording()
        self.assertEqual(summary["samples"], 2)
        self.assertAlmostEqual(calls[1] - calls[0], 0.15)

    def test_bad_responses_are_recorded_and_do_not_hide_recovery(self) -> None:
        invalid = [b"[]", b'{"stale": NaN}', {**state(), "agents": [4]},
                   {**state(), "configured": "true"}, {**state(), "age_ms": -1}]
        self.responses = [(200, body) for body in invalid] + [(0, b""), (200, state())]
        code, summary, _ = self.run_recording()
        self.assertEqual(code, 0)
        self.assertEqual(summary["counts"], {"error": len(invalid) + 1, "fresh": 1})

    def test_oversized_response_is_bounded(self) -> None:
        self.responses = [(200, b"x" * 100)]
        with patch.object(record, "MAX_RESPONSE_BYTES", 32):
            with self.assertRaisesRegex(ValueError, "exceeds"):
                record.fetch_state(self.args.url, 0.5)

    def test_timeout_is_recorded_and_sampling_recovers(self) -> None:
        def recovered(url: str, timeout: float) -> dict:
            self.stop.set()
            return state()

        calls = 0

        def fetch(url: str, timeout: float) -> dict:
            nonlocal calls
            calls += 1
            if calls == 1:
                raise TimeoutError()
            return recovered(url, timeout)

        with patch.object(record, "fetch_state", side_effect=fetch):
            code, summary, rows = self.run_recording()
        self.assertEqual(code, 0)
        self.assertEqual(summary["counts"], {"error": 1, "fresh": 1})
        self.assertEqual(next(r for r in rows if r.get("status") == "error")["error"]["type"], "TimeoutError")

    def test_new_sessions_never_overwrite_old_files(self) -> None:
        self.responses = [(200, state())]
        self.run_recording()
        previous = {path: path.read_bytes() for path in Path(self.temp.name).rglob("*") if path.is_file()}
        self.stop.clear()
        self.responses = [(200, state())]
        with contextlib.redirect_stdout(io.StringIO()):
            record.record(self.args, self.stop)
        self.assertEqual(len(list(Path(self.temp.name).iterdir())), 2)
        for path, content in previous.items():
            self.assertEqual(path.read_bytes(), content)

    def test_cli_rejects_invalid_options_before_creating_files(self) -> None:
        for option in ("--hours", "--interval", "--timeout", "--max-part-mib"):
            for value in ("0", "-1", "nan", "inf"):
                with self.subTest(option=option, value=value), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        record.main([option, value, "--output", self.temp.name])
                    self.assertEqual(error.exception.code, 2)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            record.main(["--url", "http://user:secret@localhost/api/state", "--output", self.temp.name])
        self.assertEqual(list(Path(self.temp.name).iterdir()), [])

    def test_cli_signal_handler_finishes_and_restores_previous_handler(self) -> None:
        previous = signal.getsignal(signal.SIGTERM)

        def fetch_and_stop(url: str, timeout: float) -> dict:
            signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
            return state()

        with (patch.object(record, "fetch_state", side_effect=fetch_and_stop),
              contextlib.redirect_stdout(io.StringIO())):
            code = record.main(["--output", self.temp.name, "--hours", "4"])
        self.assertEqual(code, 0)
        self.assertEqual(signal.getsignal(signal.SIGTERM), previous)
        summary_path = next(Path(self.temp.name).glob("*/summary.json"))
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        self.assertEqual(summary["stop_reason"], "stopped")
        with gzip.open(summary_path.parent / summary["parts"][-1], "rt", encoding="utf-8") as handle:
            rows = [json.loads(line) for line in handle]
        self.assertEqual(rows[-1]["kind"], "summary")

    @unittest.skipIf(sys.platform == "win32", "Windows terminate() does not deliver POSIX SIGTERM")
    def test_sigterm_finalizes_recording(self) -> None:
        process = subprocess.Popen([sys.executable, str(Path(record.__file__)), "--url", self.args.url,
                                    "--output", self.temp.name, "--hours", "4", "--interval", "60"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not self.requests and time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.01)
            self.assertTrue(self.requests)
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
            summary_path = next(Path(self.temp.name).glob("*/summary.json"))
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["stop_reason"], "stopped")
            for part in summary["parts"]:
                with gzip.open(summary_path.parent / part, "rt", encoding="utf-8") as handle:
                    rows = [json.loads(line) for line in handle]
                self.assertEqual(rows[-1]["kind"], "summary")
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()


if __name__ == "__main__":
    unittest.main()
