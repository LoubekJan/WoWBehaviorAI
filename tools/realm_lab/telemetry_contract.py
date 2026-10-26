"""Deployment population checks, separate from physical behavior acceptance."""
from __future__ import annotations

import math
import json
import os
import time
import urllib.request

try:
    from . import consumer_population as consumers, settlement_population as settlement, supply_population as supply, wildlife_population as wildlife
except ImportError:
    import consumer_population as consumers
    import settlement_population as settlement
    import supply_population as supply
    import wildlife_population as wildlife


def validate_snapshot(state: dict, profile: str, wildlife_enabled: bool = True) -> dict[str, int]:
    require = settlement.require
    require(profile in (settlement.PROFILE, wildlife.PROFILE, supply.PROFILE, consumers.PROFILE), "Unknown authored-world telemetry profile")
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
    active = {wildlife.PROFILE: wildlife.SPAWN_IDS, supply.PROFILE: supply.SPAWN_IDS,
              consumers.PROFILE: consumers.SPAWN_IDS}.get(profile, frozenset()) if wildlife_enabled else frozenset()
    live_roles = 0
    for actor in actors:
        identity = actor["spawn_id"]
        require(actor.get("agent_id") == identity and (actor.get("position") or {}).get("map_id") == 725,
                "Telemetry actor identity or map differs")
        if identity not in active:
            require(actor.get("native_creature") is True and actor.get("control_mode") == "OBSERVE_ONLY" and
                    actor.get("needs") is None and actor.get("economy") is None and actor.get("living_role") is None,
                    "Native settlement NPC acquired AI telemetry or ownership")
            require(actor.get("supply") is None, "Native settlement NPC acquired a supply role")
            continue
        require(actor.get("native_creature") is False and actor.get("control_mode") == "AI_WORLD_CONTROLLED",
                "Selected wildlife is not AI controlled")
        needs = actor.get("needs")
        require(isinstance(needs, dict) and type(needs.get("hunger")) in (int, float) and
                math.isfinite(needs["hunger"]) and 0 <= needs["hunger"] <= 1,
                "Controlled wildlife needs are unavailable or invalid")
        role = actor.get("living_role")
        if actor.get("alive") is True and (actor.get("position") or {}).get("source") == "live":
            expected_role = consumers.CONSUMER_ROLES.get(identity) or ("HUNTER" if identity in supply.HUNTER_IDS else
                "PREDATOR" if identity in wildlife.PREDATOR_IDS else "PREY")
            require(isinstance(role, dict) and role.get("enabled") is True and role.get("extensions_enabled") is True and
                    role.get("role") == expected_role,
                    "Live wildlife role is disabled or misclassified")
            require(isinstance(actor.get("runtime_guid"), str) and 0 < len(actor["runtime_guid"]) <= 80 and
                    type(role.get("lifetime_ms")) is int and role["lifetime_ms"] > 0,
                    "Live wildlife incarnation telemetry is missing")
            if identity in supply.HUNTER_IDS:
                supplied = actor.get("supply")
                depot_id = 1 if identity == 901034 else 2
                require(isinstance(supplied, dict) and supplied.get("enabled") is True and
                        supplied.get("role") == "HUNTER" and supplied.get("depot_id") == depot_id,
                        "Live supply hunter capability or assigned depot is missing")
                cargo, capacity = supplied.get("cargo"), supplied.get("capacity")
                require(type(cargo) is int and type(capacity) is int and 0 <= cargo <= capacity <= 6 and capacity > 0,
                        "Live supply hunter cargo is unavailable or unbounded")
                depot = supplied.get("depot")
                require(isinstance(depot, dict) and depot.get("id") == depot_id and
                        (depot.get("position") or {}).get("map_id") == 725 and
                        type(depot.get("stock")) is int and type(depot.get("capacity")) is int and
                        0 <= depot["stock"] <= depot["capacity"] and depot["capacity"] > 0,
                        "Live supply depot snapshot is missing or unbounded")
                require(isinstance(supplied.get("accounting"), dict) and
                        supplied["accounting"].get("conserved") is True,
                        "Supply food conservation invariant failed or is unavailable")
            elif identity in consumers.CONSUMER_IDS:
                expected_type = "CIVILIAN" if consumers.CONSUMER_TYPES[identity] == 0 else "COMBATANT"
                require(actor.get("type") == expected_type and type(actor.get("world_faction")) is int and
                        actor["world_faction"] == consumers.CONSUMER_FACTIONS[identity],
                        "Consumer ordinary type or faction changed")
                supplied = actor.get("supply")
                depot_id = consumers.CONSUMER_DEPOTS[identity]
                require(isinstance(supplied, dict) and supplied.get("enabled") is True and
                        supplied.get("role") == "CONSUMER" and supplied.get("depot_id") == depot_id and
                        type(supplied.get("cargo")) is int and supplied["cargo"] == 0 and
                        type(supplied.get("capacity")) is int and supplied["capacity"] == 0,
                        "Consumer depot capability must not acquire hunting or cargo")
                depot = supplied.get("depot")
                require(isinstance(depot, dict) and depot.get("id") == depot_id and
                        (depot.get("position") or {}).get("map_id") == 725 and
                        type(depot.get("stock")) is int and type(depot.get("capacity")) is int and
                        0 <= depot["stock"] <= depot["capacity"] and depot["capacity"] > 0,
                        "Consumer depot snapshot is missing or unbounded")
                require(isinstance(supplied.get("accounting"), dict) and
                        supplied["accounting"].get("conserved") is True,
                        "Consumer food conservation invariant failed or is unavailable")
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
