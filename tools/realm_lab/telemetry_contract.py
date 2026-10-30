"""Deployment population checks, separate from physical behavior acceptance."""
from __future__ import annotations

import math
import json
import os
import time
import urllib.request

try:
    from . import defias_population as defias, field_population as fields, raid_population as raids, consumer_population as consumers, settlement_population as settlement, supply_population as supply, wildlife_population as wildlife
except ImportError:
    import defias_population as defias
    import field_population as fields
    import consumer_population as consumers
    import raid_population as raids
    import settlement_population as settlement
    import supply_population as supply
    import wildlife_population as wildlife


def validate_snapshot(state: dict, profile: str, wildlife_enabled: bool = True) -> dict[str, int]:
    require = settlement.require
    require(profile in (settlement.PROFILE, wildlife.PROFILE, supply.PROFILE, consumers.PROFILE, raids.PROFILE, fields.PROFILE, defias.PROFILE), "Unknown authored-world telemetry profile")
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
              consumers.PROFILE: consumers.SPAWN_IDS, raids.PROFILE: raids.SPAWN_IDS,
              fields.PROFILE: fields.SPAWN_IDS, defias.PROFILE: defias.SPAWN_IDS}.get(profile, frozenset()) if wildlife_enabled else frozenset()
    defias_enabled = profile == defias.PROFILE and wildlife_enabled
    field_population = fields.Population.load() if profile in (fields.PROFILE, defias.PROFILE) and wildlife_enabled else None
    field_actors = {actor.spawn_id: actor for actor in field_population.recruits} if field_population else {}
    field_definitions = {field["id"]: field for field in field_population.fields} if field_population else {}
    raider_ids = defias.RAIDER_IDS if defias_enabled else raids.RAIDER_IDS
    raid_actor_ids = raider_ids | raids.GUARD_IDS
    camp_consumers = defias.GUARD_IDS | defias.SUPPLIER_IDS if defias_enabled else frozenset()
    village_warehouses = {1: supply.DEPOTS[0], **{key: value["warehouse"] for key, value in field_definitions.items()}}
    accounting_snapshot, field_snapshots, warehouse_snapshots = None, {}, {}
    raid_warehouse_snapshots, camp_snapshot = {}, None
    live_roles = 0
    for actor in actors:
        identity = actor["spawn_id"]
        require(actor.get("agent_id") == identity and (actor.get("position") or {}).get("map_id") == 725,
                "Telemetry actor identity or map differs")
        if profile not in (raids.PROFILE, fields.PROFILE, defias.PROFILE) or identity not in raid_actor_ids or not wildlife_enabled:
            require(actor.get("raid") is None, "Unselected actor acquired a raid role")
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
            expected_role = (defias.RECRUIT_ROLES.get(identity) if defias_enabled else None) or ("FARMER" if identity in field_actors else raids.RECRUIT_ROLES.get(identity)) or consumers.CONSUMER_ROLES.get(identity) or ("HUNTER" if identity in supply.HUNTER_IDS else
                "PREDATOR" if identity in wildlife.PREDATOR_IDS else "PREY")
            require(isinstance(role, dict) and role.get("enabled") is True and role.get("extensions_enabled") is True and
                    role.get("role") == expected_role,
                    "Live wildlife role is disabled or misclassified")
            require(isinstance(actor.get("runtime_guid"), str) and 0 < len(actor["runtime_guid"]) <= 80 and
                    type(role.get("lifetime_ms")) is int and role["lifetime_ms"] > 0,
                    "Live wildlife incarnation telemetry is missing")
            if identity in raid_actor_ids:
                require(actor.get("type") == ("GUARD" if identity in raids.GUARD_IDS else "COMBATANT") and
                        type(actor.get("world_faction")) is int and actor["world_faction"] == (1 if identity in raids.GUARD_IDS else 2),
                        "Raid ordinary type or faction changed")
                raid = actor.get("raid")
                source_id = raid.get("source_depot_id") if isinstance(raid, dict) else None
                allowed_sources = village_warehouses if identity in raider_ids else {1: supply.DEPOTS[0]}
                require(isinstance(raid, dict) and raid.get("enabled") is True and
                        raid.get("role") == ("RAIDER" if identity in raider_ids else "GUARD") and
                        type(source_id) is int and source_id in allowed_sources and raid.get("camp_depot_id") == 3,
                        "Raid capability or reviewed depot assignment is missing")
                for key, expected in (("source_depot", allowed_sources[source_id]), ("camp_depot", raids.DEPOT)):
                    depot = raid.get(key)
                    require(isinstance(depot, dict) and depot.get("id") == expected["id"] and
                            isinstance(depot.get("position"), dict) and
                            all(type(depot["position"].get(field)) in (int, float) and
                                math.isfinite(depot["position"][field]) and
                                abs(depot["position"][field]-value) <= 0.001
                                for field, value in expected["position"].items()) and
                            type(depot.get("stock")) is int and type(depot.get("capacity")) is int and
                            0 <= depot["stock"] <= depot["capacity"] == 30,
                            "Raid depot coordinates, stock or capacity changed")
                require(source_id not in raid_warehouse_snapshots or raid_warehouse_snapshots[source_id] == raid["source_depot"],
                        "Raiders report conflicting copies of the same warehouse")
                raid_warehouse_snapshots[source_id] = raid["source_depot"]
                require(camp_snapshot is None or camp_snapshot == raid["camp_depot"],
                        "Raiders report conflicting copies of the Defias camp")
                camp_snapshot = raid["camp_depot"]
            if identity in field_actors:
                farmer = field_actors[identity]
                definition = field_definitions[farmer.field_id]
                supplied = actor.get("supply")
                require(actor.get("type") == "CIVILIAN" and type(actor.get("world_faction")) is int and
                        actor["world_faction"] == 1 and isinstance(supplied, dict) and
                        supplied.get("enabled") is True and supplied.get("role") == "FARMER" and
                        type(supplied.get("field_id")) is int and supplied["field_id"] == farmer.field_id and
                        type(supplied.get("depot_id")) is int and supplied["depot_id"] == farmer.depot_id and
                        type(supplied.get("cargo")) is int and type(supplied.get("capacity")) is int and
                        0 <= supplied["cargo"] <= supplied["capacity"] == 2 and
                        all(supplied.get(key) is None for key in
                            ("hunting_ground", "carcass_remaining", "carcass_spawn_id", "carcass_life")),
                        "Farmer type, faction, field assignment, cargo or non-hunting capability changed")
                point = supplied.get("field_workpoint_index")
                require(type(point) is int and point in tuple(farmer.worker_ordinal + 4*c for c in range(3)) and
                        isinstance(supplied.get("field_workpoint"), dict) and
                        all(type(supplied["field_workpoint"].get(key)) in (int, float) and
                            math.isfinite(supplied["field_workpoint"][key]) and
                            abs(supplied["field_workpoint"][key] - definition["work_points"][point][key]) <= .001
                            for key in ("map_id", "x", "y", "z")) and
                        type(supplied.get("field_remaining")) is int and type(supplied.get("field_capacity")) is int and
                        0 <= supplied["field_remaining"] <= supplied["field_capacity"] == 24,
                        "Farmer work point rotation or bounded crop potential changed")
                depot = supplied.get("depot")
                require(isinstance(depot, dict) and depot.get("id") == farmer.depot_id and
                        isinstance(depot.get("position"), dict) and
                        all(type(depot["position"].get(key)) in (int, float) and math.isfinite(depot["position"][key]) and
                            abs(depot["position"][key] - expected) <= .001
                            for key, expected in definition["warehouse"]["position"].items()) and
                        type(depot.get("stock")) is int and type(depot.get("capacity")) is int and
                        type(depot.get("low_threshold")) is int and depot["low_threshold"] == 4 and
                        0 <= depot["stock"] <= depot["capacity"] == 30 and
                        isinstance(supplied.get("accounting"), dict) and supplied["accounting"].get("conserved") is True,
                        "Farmer own warehouse, stock or food conservation changed")
                field_snapshot = (supplied["field_remaining"], supplied["field_capacity"])
                require(farmer.field_id not in field_snapshots or field_snapshots[farmer.field_id] == field_snapshot,
                        "Conflicting copies of the same crop field")
                require(farmer.depot_id not in warehouse_snapshots or warehouse_snapshots[farmer.depot_id] == depot,
                        "Conflicting copies of the same farmer warehouse")
                field_snapshots[farmer.field_id], warehouse_snapshots[farmer.depot_id] = field_snapshot, depot
            elif identity in raider_ids:
                supplied = actor.get("supply")
                require(isinstance(supplied, dict) and supplied.get("enabled") is True and
                        supplied.get("role") == "RAIDER" and supplied.get("depot_id") == 3 and
                        type(supplied.get("cargo")) is int and type(supplied.get("capacity")) is int and
                        0 <= supplied["cargo"] <= supplied["capacity"] == 2 and
                        all(supplied.get(field) is None for field in
                            ("hunting_ground", "carcass_remaining", "carcass_spawn_id", "carcass_life")),
                        "Raider cargo, depot or non-hunting capability changed")
                require(supplied.get("depot") == actor["raid"]["camp_depot"] and
                        isinstance(supplied.get("accounting"), dict) and
                        supplied["accounting"].get("conserved") is True,
                        "Raider camp ledger is missing or not conserved")
            elif identity in supply.HUNTER_IDS:
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
            elif identity in consumers.CONSUMER_IDS | raids.GUARD_IDS | camp_consumers:
                guard = identity in raids.GUARD_IDS
                camp_consumer = identity in camp_consumers
                expected_type = "GUARD" if guard or identity in defias.GUARD_IDS else "CIVILIAN" if (defias.RECRUIT_TYPES[identity] if camp_consumer else consumers.CONSUMER_TYPES[identity]) == 0 else "COMBATANT"
                require(actor.get("type") == expected_type and type(actor.get("world_faction")) is int and
                        actor["world_faction"] == (2 if camp_consumer else 1 if guard else consumers.CONSUMER_FACTIONS[identity]),
                        "Consumer ordinary type or faction changed")
                supplied = actor.get("supply")
                depot_id = 3 if camp_consumer else 1 if guard else consumers.CONSUMER_DEPOTS[identity]
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
                if camp_consumer:
                    require(depot.get("position") == raids.DEPOT["position"] and depot.get("capacity") == 30 and
                            (camp_snapshot is None or depot == camp_snapshot), "Defias consumer camp snapshot changed or conflicts")
                    camp_snapshot = depot
            live_roles += 1
            if field_population and isinstance(actor.get("supply"), dict):
                accounting = actor["supply"].get("accounting")
                counter_fields = ("initial_food", "harvested", "in_depots", "in_cargo", "consumed", "lost")
                require(isinstance(accounting, dict) and all(isinstance(accounting.get(key), str) and
                        0 < len(accounting[key]) <= 20 and accounting[key].isascii() and accounting[key].isdigit() and
                        str(int(accounting[key])) == accounting[key] and 0 <= int(accounting[key]) < 2**64
                        for key in counter_fields) and accounting.get("conserved") is True and
                        int(accounting["initial_food"]) + int(accounting["harvested"]) == sum(
                            int(accounting[key]) for key in ("in_depots", "in_cargo", "consumed", "lost")),
                        "Field profile food accounting is missing, invalid or not conserved")
                require(accounting_snapshot is None or accounting_snapshot == accounting,
                        "Field profile supply roles do not share one food accounting snapshot")
                accounting_snapshot = accounting
    for identity, depot in raid_warehouse_snapshots.items():
        require(identity not in warehouse_snapshots or warehouse_snapshots[identity] == depot,
                "Raid source copy disagrees with the village warehouse ledger")
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
