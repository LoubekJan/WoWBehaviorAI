"""Deployment population checks, separate from physical behavior acceptance."""
from __future__ import annotations

import math
import json
import os
import time
import urllib.request

try:
    from . import settlement_population as settlement, wildlife_population as wildlife
except ImportError:
    import settlement_population as settlement
    import wildlife_population as wildlife


def validate_snapshot(state: dict, profile: str, wildlife_enabled: bool = True) -> dict[str, int]:
    require = settlement.require
    require(profile in (settlement.PROFILE, wildlife.PROFILE), "Unknown authored-world telemetry profile")
    require(isinstance(state, dict) and state.get("stale") is False and state.get("version") == 4,
            "Authored-world telemetry must be a fresh version-4 snapshot")
    require(type(state.get("captured_at_ms")) is int and state["captured_at_ms"] > 0,
            "Telemetry capture timestamp is missing")
    actors = state.get("agents")
    require(isinstance(actors, list) and len(actors) == 100 and all(isinstance(a, dict) for a in actors),
            "Authored-world telemetry must contain exactly 100 actors")
    ids = [a.get("spawn_id") for a in actors]
    require(all(type(identity) is int for identity in ids) and set(ids) == settlement.SPAWN_IDS,
            "Telemetry population differs from the reviewed 100 spawns")
    active = wildlife.SPAWN_IDS if profile == wildlife.PROFILE and wildlife_enabled else frozenset()
    live_roles = 0
    for actor in actors:
        identity = actor["spawn_id"]
        require(actor.get("agent_id") == identity and (actor.get("position") or {}).get("map_id") == 725,
                "Telemetry actor identity or map differs")
        if identity not in active:
            require(actor.get("native_creature") is True and actor.get("control_mode") == "OBSERVE_ONLY" and
                    actor.get("needs") is None and actor.get("economy") is None and actor.get("living_role") is None,
                    "Native settlement NPC acquired AI telemetry or ownership")
            continue
        require(actor.get("native_creature") is False and actor.get("control_mode") == "AI_WORLD_CONTROLLED",
                "Selected wildlife is not AI controlled")
        needs = actor.get("needs")
        require(isinstance(needs, dict) and type(needs.get("hunger")) in (int, float) and
                math.isfinite(needs["hunger"]) and 0 <= needs["hunger"] <= 1,
                "Controlled wildlife needs are unavailable or invalid")
        role = actor.get("living_role")
        if actor.get("alive") is True and (actor.get("position") or {}).get("source") == "live":
            require(isinstance(role, dict) and role.get("enabled") is True and role.get("extensions_enabled") is True and
                    role.get("role") == ("PREDATOR" if identity in wildlife.PREDATOR_IDS else "PREY"),
                    "Live wildlife role is disabled or misclassified")
            require(isinstance(actor.get("runtime_guid"), str) and 0 < len(actor["runtime_guid"]) <= 80 and
                    type(role.get("lifetime_ms")) is int and role["lifetime_ms"] > 0,
                    "Live wildlife incarnation telemetry is missing")
            live_roles += 1
    require(not active or live_roles > 0, "No living controlled wildlife role is observable")
    return {"total": 100, "managed": len(active), "native": 100 - len(active), "live_roles": live_roles}


def wait_for_snapshots(url: str, profile: str, wildlife_enabled: bool = True) -> dict[str, int]:
    previous = None
    last = "no advancing valid snapshot"
    for attempt in range(60):
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                state = json.load(response)
            counts = validate_snapshot(state, profile, wildlife_enabled)
            captured = state["captured_at_ms"]
            if previous is not None and captured > previous:
                return counts
            previous = captured
        except (settlement.ProfileError, OSError, ValueError, KeyError, TypeError) as error:
            previous = None
            last = str(error) if isinstance(error, settlement.ProfileError) else "telemetry unavailable"
        time.sleep(1)
    raise settlement.ProfileError("Authored-world telemetry did not produce two complete fresh snapshots: " + last)


if __name__ == "__main__":
    try:
        try:
            from .manage import Settings
        except ImportError:
            from manage import Settings
        settings = Settings.load(os.environ)
        print(json.dumps(wait_for_snapshots("http://world-viewer:8000/api/state", settings.ai_profile,
                                           settings.wildlife_enabled)))
    except (settlement.ProfileError, ValueError) as error:
        raise SystemExit(str(error))
