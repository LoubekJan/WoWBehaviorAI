"""Record the public Observer API as gzip JSONL; Python standard library only."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import gzip
from http.client import HTTPException
import json
import math
from pathlib import Path
import signal
import sys
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import uuid

if __package__:
    from .behavior import Evaluator, Policy, write_report
else:
    from behavior import Evaluator, Policy, write_report


FORMAT_VERSION = 1
MAX_RESPONSE_BYTES = 64 * 1024 * 1024  # GET expands defaults from the bounded POST.


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def positive_number(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return number


def reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number: {value}")


def fetch_state(url: str, timeout: float) -> dict:
    started = time.monotonic()
    request = Request(url, headers={"Accept": "application/json", "Cache-Control": "no-cache"})
    with urlopen(request, timeout=timeout) as response:
        chunks = []
        size = 0
        while True:
            chunk = response.read1(min(65536, MAX_RESPONSE_BYTES + 1 - size))
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
            if size > MAX_RESPONSE_BYTES:
                raise ValueError("Observer response exceeds 64 MiB")
            if time.monotonic() - started >= timeout:
                raise TimeoutError("Observer response exceeded request deadline")
    state = json.loads(b"".join(chunks), parse_constant=reject_constant)
    if not isinstance(state, dict):
        raise ValueError("Observer state must be an object")
    for field in ("configured", "stale"):
        if type(state.get(field)) is not bool:
            raise ValueError(f"Observer state missing boolean {field}")
    if type(state.get("version")) is not int or state["version"] < 1:
        raise ValueError("Observer state missing protocol version")
    if not isinstance(state.get("agents"), list) or any(not isinstance(a, dict) for a in state["agents"]):
        raise ValueError("Observer state missing agents array")
    for field in ("captured_at_ms", "received_at_ms", "age_ms"):
        if field not in state or (state[field] is not None and
                                  (type(state[field]) is not int or state[field] < 0)):
            raise ValueError(f"Observer state has invalid {field}")
    return state


def classify(state: dict) -> str:
    if not state["configured"]:
        return "unconfigured"
    if any(state[field] is None for field in ("captured_at_ms", "received_at_ms", "age_ms")):
        return "waiting"
    if state["stale"]:
        return "stale"
    return "fresh" if state["agents"] else "empty"


class Recording:
    """Each part is independently readable and starts with session metadata."""

    def __init__(self, output: Path, metadata: dict, max_part_bytes: int):
        session_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
        self.directory = output / ("aiworld-" + session_id)
        self.directory.mkdir(parents=True, exist_ok=False)
        self.metadata = {"kind": "session", "format_version": FORMAT_VERSION,
                         "session_id": session_id, **metadata}
        self.max_part_bytes = max_part_bytes
        self.parts: list[str] = []
        self.raw = None
        self.stream = None

    def _write_line(self, record: dict) -> None:
        self.stream.write((json.dumps(record, ensure_ascii=False, separators=(",", ":"),
                                      allow_nan=False) + "\n").encode("utf-8"))
        self.stream.flush()
        self.raw.flush()

    def write(self, record: dict) -> None:
        if self.raw is None or self.raw.tell() >= self.max_part_bytes:
            self.close()
            name = f"part-{len(self.parts) + 1:04d}.jsonl.gz"
            self.raw = (self.directory / name).open("xb")
            self.stream = gzip.GzipFile(filename="", mode="wb", fileobj=self.raw, compresslevel=3, mtime=0)
            self.parts.append(name)
            self._write_line({**self.metadata, "part": len(self.parts)})
        self._write_line(record)

    def save_summary(self, summary: dict) -> None:
        # Readers always see either the previous or the new complete summary.
        temporary = self.directory / "summary.json.tmp"
        temporary.write_text(json.dumps({**self.metadata, **summary, "parts": self.parts},
                                        ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        temporary.replace(self.directory / "summary.json")

    def close(self) -> None:
        if self.stream is not None:
            try:
                self.stream.close()
            finally:
                self.raw.close()
                self.stream = None
                self.raw = None


def record(args: argparse.Namespace, stop: threading.Event) -> int:
    started = time.monotonic()
    deadline = started + args.hours * 3600
    recording = Recording(args.output, {
        "started_at_utc": utc_now(), "source": args.url, "label": args.label,
        "interval_seconds": args.interval, "planned_hours": args.hours,
        "max_part_mib": args.max_part_mib,
        "build_label": getattr(args, "build_label", "unknown"),
    }, max(1, int(args.max_part_mib * 1024 * 1024)))
    evaluator = (Evaluator(recording.metadata, Policy(minimum_seconds=getattr(args, "minimum_minutes", 60) * 60))
                 if getattr(args, "analyze", False) else None)
    counts: Counter = Counter()
    samples = 0
    max_agents = 0
    last_status = None
    next_summary = started
    reason = "duration"
    print(f"Recording to {recording.directory}", flush=True)

    def summary() -> dict:
        return {"kind": "summary", "updated_at_utc": utc_now(),
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "samples": samples, "counts": dict(counts), "max_agents": max_agents,
                "last_status": last_status}

    try:
        recording.save_summary({**summary(), "status": "running"})
        next_sample = started
        while not stop.is_set() and time.monotonic() < deadline:
            wait_seconds = min(next_sample, deadline) - time.monotonic()
            if wait_seconds > 0:
                stop.wait(wait_seconds)
                # Timed waits may return early; recheck both clocks before polling.
                continue
            samples += 1
            sample = {"kind": "sample", "sequence": samples, "recorded_at_utc": utc_now(),
                      "elapsed_seconds": round(time.monotonic() - started, 3)}
            request_started = time.monotonic()
            try:
                state = fetch_state(args.url, min(args.timeout, max(0.001, deadline - request_started)))
                status = classify(state)
                sample.update(status=status, state=state)
                max_agents = max(max_agents, len(state["agents"]))
            except (HTTPError, URLError, HTTPException, OSError, ValueError) as error:
                status = "error"
                # Do not persist response bodies or URL-bearing exception text.
                sample.update(status=status, error={"type": type(error).__name__})
                if isinstance(error, HTTPError):
                    sample["error"]["http_status"] = error.code
                    error.close()
            sample["request_ms"] = round((time.monotonic() - request_started) * 1000)
            counts[status] += 1
            recording.write(sample)
            if evaluator:
                evaluator.observe(sample)
            changed = status != last_status
            last_status = status
            if changed or time.monotonic() >= next_summary:
                progress = {**summary(), "status": "running"}
                recording.save_summary(progress)
                print(json.dumps(progress), flush=True)
                next_summary = time.monotonic() + 60
            # Skip missed slots after a slow request/write; never burst to catch up.
            next_sample += args.interval
            now = time.monotonic()
            if next_sample <= now:
                next_sample += (math.floor((now - next_sample) / args.interval) + 1) * args.interval
        if stop.is_set():
            reason = "stopped"
    except BaseException:
        reason = "failed"
        raise
    finally:
        # Close gzip even when disk/write errors prevent a final summary.
        final = {**summary(), "status": "finished", "stop_reason": reason,
                 "usable": counts["fresh"] > 0}
        try:
            recording.write(final)
        finally:
            recording.close()
        recording.save_summary(final)
        print(json.dumps(final), flush=True)
    if evaluator:
        report = evaluator.finish(final)
        write_report(report, recording.directory)
        result = {"status": report["status"], "exit_code": report["exit_code"],
                  "report": "behavior-report.json", "findings": len(report["findings"])}
        recording.save_summary({**final, "behavior": result})
        print(json.dumps({"behavior": result}), flush=True)
        return report["exit_code"]
    # An empty/unconfigured/offline recording must not report a successful test.
    return 0 if counts["fresh"] else 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://localhost:8090/api/state")
    parser.add_argument("--output", type=Path, default=Path("runtime/recordings"))
    parser.add_argument("--hours", type=positive_number, default=4.0)
    parser.add_argument("--interval", type=positive_number, default=5.0)
    parser.add_argument("--timeout", type=positive_number, default=3.0)
    parser.add_argument("--max-part-mib", type=positive_number, default=64.0)
    parser.add_argument("--label", default="Elwynn")
    parser.add_argument("--build-label", default="unknown", help="label of the tested server build")
    parser.add_argument("--analyze", action="store_true", help="evaluate behavior and write a final report")
    parser.add_argument("--minimum-minutes", type=positive_number, default=60.0,
                        help="minimum fresh observation time required by behavior checks")
    args = parser.parse_args(argv)
    endpoint = urlsplit(args.url)
    if (endpoint.scheme not in ("http", "https") or not endpoint.hostname or endpoint.username or
            endpoint.password or endpoint.query or endpoint.fragment):
        parser.error("--url must be an HTTP(S) endpoint without credentials, query or fragment")
    stop = threading.Event()

    def shutdown(signum: int, frame: object) -> None:
        stop.set()

    previous = {sig: signal.signal(sig, shutdown) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        return record(args, stop)
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


if __name__ == "__main__":
    sys.exit(main())
