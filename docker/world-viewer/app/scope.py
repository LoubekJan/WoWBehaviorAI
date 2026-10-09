"""Public Observer scope configuration, independent of the telemetry wire format."""
from __future__ import annotations

import json
import math
import os
from collections.abc import Mapping


def normalize_scope(value: dict | None = None) -> dict:
    if value is None:
        value = {"map_id": 0, "zone_ids": [12], "bounds": None, "name": "Elwynn Forest"}
    if not isinstance(value, dict):
        raise ValueError("Observer scope must be an object")
    map_id = value.get("map_id")
    zones = value.get("zone_ids")
    name = value.get("name")
    if type(map_id) is not int or not 0 <= map_id <= 2**32 - 1:
        raise ValueError("Observer scope has an invalid map_id")
    if (not isinstance(zones, list) or not zones or len(zones) > 64 or
            any(type(zone) is not int or not 0 < zone <= 2**32 - 1 for zone in zones) or
            len(zones) != len(set(zones))):
        raise ValueError("Observer scope has invalid zone_ids")
    if not isinstance(name, str) or not name.strip() or len(name) > 200:
        raise ValueError("Observer scope has an invalid name")
    bounds = value.get("bounds")
    if bounds is not None:
        keys = {"min_x", "max_x", "min_y", "max_y"}
        if (not isinstance(bounds, dict) or set(bounds) != keys or
                any(type(bounds[key]) not in (int, float) or not math.isfinite(bounds[key]) for key in keys) or
                bounds["min_x"] >= bounds["max_x"] or bounds["min_y"] >= bounds["max_y"]):
            raise ValueError("Observer scope has invalid bounds")
        bounds = {key: bounds[key] for key in ("min_x", "max_x", "min_y", "max_y")}
    return {"map_id": map_id, "zone_ids": sorted(zones), "bounds": bounds, "name": name}


def scope_from_environment(env: Mapping[str, str] | None = None) -> dict:
    env = os.environ if env is None else env
    raw_bounds = env.get("WORLD_VIEWER_SCOPE_BOUNDS", "").strip()
    try:
        return normalize_scope({
            "map_id": int(env.get("WORLD_VIEWER_SCOPE_MAP_ID", "0")),
            "zone_ids": [int(zone.strip()) for zone in env.get("WORLD_VIEWER_SCOPE_ZONE_IDS", "12").split(",")],
            "bounds": json.loads(raw_bounds) if raw_bounds else None,
            "name": env.get("WORLD_VIEWER_SCOPE_NAME", "Elwynn Forest"),
        })
    except (ValueError, TypeError) as error:
        raise ValueError("Invalid Observer scope environment") from error


def contains_position(scope: dict, position: dict) -> bool:
    if position.get("map_id") != scope["map_id"]:
        return False
    bounds = scope["bounds"]
    if bounds is None:
        return True  # Historical Elwynn evaluation was map-only.
    x, y = position.get("x"), position.get("y")
    return (type(x) in (int, float) and type(y) in (int, float) and math.isfinite(x) and math.isfinite(y)
            and bounds["min_x"] <= x <= bounds["max_x"] and bounds["min_y"] <= y <= bounds["max_y"])
