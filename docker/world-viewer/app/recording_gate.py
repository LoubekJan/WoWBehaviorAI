"""Bounded deployment checks before and after starting the detached recorder."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from http.client import HTTPException
import json
from pathlib import Path
import sys
import time

if __package__:
    from .record import classify, fetch_state, positive_number
else:
    from record import classify, fetch_state, positive_number


def has_live_roles(state: dict) -> bool:
    if (classify(state) != "fresh" or state["version"] not in (2, 3, 4)
            or state["age_ms"] > 5000):
        return False
    for agent in state["agents"]:
        position = agent.get("position") or {}
        role = agent.get("living_role") or {}
        if (isinstance(position, dict) and isinstance(role, dict)
                and agent.get("control_mode") == "AI_WORLD_CONTROLLED" and agent.get("alive") is True
                and position.get("source") == "live" and position.get("map_id") == 0
                and role.get("enabled") is True and not agent.get("living_wolf")
                and role.get("status") in {"READY", "ACTIVE", "CURATED_ROUTINE", "GROUP_ACTIVITY"}):
            return True
    return False


def wait_for_telemetry(url: str, after_ms: int, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    previous_capture = None
    last = "waiting"
    while time.monotonic() < deadline:
        try:
            state = fetch_state(url, min(3.0, max(0.001, deadline - time.monotonic())))
            if has_live_roles(state) and state["captured_at_ms"] >= after_ms:
                capture = state["captured_at_ms"]
                if previous_capture is not None and capture > previous_capture:
                    return {"status": "ready", "captured_at_ms": capture, "agents": len(state["agents"])}
                previous_capture = capture
                last = "waiting_for_advancing_capture"
            else:
                previous_capture = None
                last = "waiting_for_new_live_roles"
        except (OSError, HTTPException, ValueError, KeyError, TypeError):
            previous_capture = None
            last = "telemetry_unavailable"
        time.sleep(min(1.0, max(0, deadline - time.monotonic())))
    raise RuntimeError(f"Telemetry did not become ready: {last}")


def utc_ms(value) -> float:
    parsed = datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        raise ValueError("recording timestamp has no timezone")
    return parsed.timestamp() * 1000


def started_session(directory: Path, build_label: str, after_ms: int, now_ms: float) -> dict | None:
    for path in sorted(directory.glob("aiworld-*/summary.json"), reverse=True):
        try:
            # A historical session of the same commit is not the new run.
            if path.stat().st_mtime * 1000 < after_ms:
                continue
            summary = json.loads(path.read_text(encoding="utf-8"))
            fresh = (summary.get("counts") or {}).get("fresh", 0)
            if (summary.get("build_label") == build_label and summary.get("status") == "running"
                    and summary.get("last_status") == "fresh" and type(fresh) is int and fresh > 0
                    and after_ms <= utc_ms(summary["started_at_utc"]) <= now_ms
                    and 0 <= now_ms - utc_ms(summary["updated_at_utc"]) <= 90000):
                return {"status": "recording", "session_id": summary["session_id"],
                        "build_label": build_label, "directory": str(path.parent), "fresh_samples": fresh}
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue  # concurrent atomic replacement or an unrelated old file
    return None


def wait_for_recording(directory: Path, build_label: str, after_ms: int, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = started_session(directory, build_label, after_ms, datetime.now(timezone.utc).timestamp() * 1000)
        if result:
            return result
        time.sleep(min(1.0, max(0, deadline - time.monotonic())))
    raise RuntimeError("The new recorder did not publish fresh samples for the deployed build")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("telemetry", "started"))
    parser.add_argument("--after-ms", type=int, required=True)
    parser.add_argument("--timeout", type=positive_number, default=180.0)
    parser.add_argument("--url", default="http://127.0.0.1:8000/api/state")
    parser.add_argument("--directory", type=Path, default=Path("/recordings"))
    parser.add_argument("--build-label")
    args = parser.parse_args(argv)
    if args.after_ms < 0:
        parser.error("--after-ms must be a nonnegative timestamp")
    if args.mode == "started" and not args.build_label:
        parser.error("--build-label is required for started")
    try:
        if args.mode == "telemetry":
            result = wait_for_telemetry(args.url, args.after_ms, args.timeout)
        else:
            result = wait_for_recording(args.directory, args.build_label, args.after_ms, args.timeout)
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
