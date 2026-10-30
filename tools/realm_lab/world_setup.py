#!/usr/bin/env python3
"""Explicit offline setup of canonical world schemas and new-character starts.

Existing characters, their saved locations and their homebinds are never edited.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

try:
    from . import full_population, defias_population, field_population, raid_population, consumer_population, manage, settlement_population, single_return, supply_population, wildlife_population
except ImportError:  # Direct invocation inside the tools container.
    import full_population, defias_population, field_population, raid_population, consumer_population, manage, settlement_population, single_return, supply_population, wildlife_population


ROOT = Path(__file__).resolve().parents[2]
START_METADATA = ROOT / "data/realm_lab/aiworldlab/world-start.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise manage.SetupError(message)


def geometry_digest(files: list | tuple) -> str:
    content = json.dumps(list(files), sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(content).hexdigest()


@dataclass(frozen=True)
class WorldStart:
    x: float
    y: float
    z: float
    orientation: float
    map_id: int = 725
    area_id: int = 4988

    @classmethod
    def load(cls, population: settlement_population.Population, metadata: Path = START_METADATA) -> WorldStart:
        try:
            document = json.loads(metadata.read_text(encoding="utf-8"))
            require(document["schema_version"] == 1 and document["source_revision"] == "preview-v8" and
                    document["map_id"] == 725 and document["area_id"] == 4988,
                    "Starting location must belong to the reviewed map 725 / area 4988")
            require(document["geometry_sha256"] == geometry_digest(population.geometry_files),
                    "Starting location proof belongs to different world geometry")
            coordinates = tuple(float(document[field]) for field in ("x", "y", "z", "orientation"))
            require(all(math.isfinite(value) for value in coordinates) and 0 <= coordinates[3] < math.tau,
                    "Invalid starting location coordinates")
            proof = document["evidence"]
            require(all(proof.get(key) is True for key in ("dry", "outside_collision_bounds", "outside_terrain_holes")) and
                    math.isfinite(float(proof["ground_z"])) and
                    0 <= coordinates[2] - float(proof["ground_z"]) <= 2.1 and
                    0 <= float(proof["slope_degrees"]) <= 15 and
                    proof["nav_ground_projection"]["flags"] == 1 and
                    abs(float(proof["nav_ground_projection"]["ground_delta"])) <= 1,
                    "Starting location lacks a dry, clear, ground navigation proof")
            start = cls(*coordinates)
            bounds = settlement_population.BOUNDS
            require(bounds["min_x"] < start.x < bounds["max_x"] and bounds["min_y"] < start.y < bounds["max_y"],
                    "Starting location is outside the authored map")
            require(all(math.hypot(actor.x - start.x, actor.y - start.y) > 45 for actor in population.actors),
                    "Starting location is too close to a native creature")
            return start
        except (KeyError, TypeError, ValueError, OSError) as exc:
            if isinstance(exc, manage.SetupError):
                raise
            raise manage.SetupError("Missing or invalid versioned starting location proof") from exc


def validate_profile(settings: manage.Settings) -> None:
    require(settings.realm_id == 2 and settings.ai_profile in (settlement_population.PROFILE, wildlife_population.PROFILE, supply_population.PROFILE, consumer_population.PROFILE, raid_population.PROFILE, field_population.PROFILE, defias_population.PROFILE, full_population.PROFILE),
            "Canonical world setup requires realm 2 with a reviewed authored-world profile")


def register_world(settings: manage.Settings) -> None:
    """Remove only an unused base-schema placeholder, never an existing realm."""
    validate_profile(settings)
    candidate = manage.mysql(settings, """SELECT JSON_OBJECT('name',name,'address',address,
 'local_address',localAddress,'subnet',localSubnetMask,'port',port,'build',gamebuild)
FROM realmlist WHERE id=1;""")
    if candidate:
        try:
            placeholder = json.loads(candidate)
        except (TypeError, ValueError) as exc:
            raise manage.SetupError("Invalid default-realm read-back") from exc
        expected = {"name": "Trinity", "address": "127.0.0.1", "local_address": "127.0.0.1",
                    "subnet": "255.255.255.0", "port": 8085, "build": 12340}
        require(placeholder == expected,
                "Realm 1 is an existing realm; use the backed-up explicit retirement migration first")
        eligible = manage.mysql(settings, """SELECT
 (SELECT COUNT(*) FROM realmcharacters WHERE realmid=1 AND numchars<>0) +
 (SELECT COUNT(*) FROM account_access WHERE RealmID=1) +
 (SELECT COUNT(*) FROM rbac_account_permissions WHERE realmId=1) +
 (SELECT COUNT(*) FROM uptime WHERE realmid=1);""")
        require(eligible == "0", "Default realm has characters, permissions or world history; automatic retirement refused")
        base_predicate = """r.id=1 AND r.name='Trinity' AND r.address='127.0.0.1'
 AND r.localAddress='127.0.0.1' AND r.localSubnetMask='255.255.255.0' AND r.port=8085 AND r.gamebuild=12340
 AND NOT EXISTS(SELECT 1 FROM account_access a WHERE a.RealmID=1)
 AND NOT EXISTS(SELECT 1 FROM rbac_account_permissions p WHERE p.realmId=1)
 AND NOT EXISTS(SELECT 1 FROM uptime u WHERE u.realmid=1)"""
        predicate = base_predicate + "\n AND NOT EXISTS(SELECT 1 FROM realmcharacters occupied WHERE occupied.realmid=1 AND occupied.numchars<>0)"
        manage.mysql(settings, f"""START TRANSACTION;
DELETE empty_counts FROM realmcharacters empty_counts INNER JOIN realmlist r ON r.id=empty_counts.realmid
WHERE {base_predicate} AND empty_counts.numchars=0;
DELETE r FROM realmlist r WHERE {predicate};
COMMIT;""")
        require(manage.mysql(settings, "SELECT COUNT(*) FROM realmlist WHERE id=1;") == "0",
                "Default realm changed during registration; automatic retirement refused")
    manage.verify_registration(settings, manage.mysql(settings, manage.register_sql(settings)))
    print("Canonical realm 2 registration verified; only an unused base-schema placeholder may be removed.")


def demote_retired_pilots(settings: manage.Settings) -> None:
    """Historical character migrations enable four pilots; the new world owns none."""
    settlement_population.ensure_world_offline()
    identities = ",".join(str(value) for value in sorted(single_return.LEGACY_PILOTS))
    manage.lab_mysql(settings, f"""START TRANSACTION;
UPDATE characters.ai_agents SET control_mode=0
WHERE map_id=0 AND agent_id=spawn_id AND spawn_id IN ({identities}) AND control_mode=1;
COMMIT;
""")
    predicate = "control_mode=1"
    if settings.ai_profile in (wildlife_population.PROFILE, supply_population.PROFILE, consumer_population.PROFILE, raid_population.PROFILE, field_population.PROFILE, defias_population.PROFILE, full_population.PROFILE):
        profile = {wildlife_population.PROFILE: wildlife_population, supply_population.PROFILE: supply_population,
                   consumer_population.PROFILE: consumer_population, raid_population.PROFILE: raid_population, field_population.PROFILE: field_population, defias_population.PROFILE: defias_population, full_population.PROFILE: full_population}[settings.ai_profile]
        owned = profile.Population.load().spawn_ids
        predicate += f" AND (agent_id NOT IN ({owned}) OR spawn_id<>agent_id OR map_id<>725)"
    remaining = manage.lab_mysql(settings, f"SELECT COUNT(*) FROM characters.ai_agents WHERE {predicate};")
    require(remaining == "0", "Unexpected managed AI agents remain; canonical native-world setup refused")


def prepare(data: Path, template: Path, binary: Path, settings: manage.Settings) -> None:
    validate_profile(settings)
    settlement_population.ensure_world_offline()
    population = settlement_population.verify_data(data)
    WorldStart.load(population)
    manage.preflight(data, settings)
    profile = {wildlife_population.PROFILE: wildlife_population, supply_population.PROFILE: supply_population,
               consumer_population.PROFILE: consumer_population, raid_population.PROFILE: raid_population, field_population.PROFILE: field_population, defias_population.PROFILE: defias_population, full_population.PROFILE: full_population}.get(settings.ai_profile)
    runtime_population = profile.Population.load() if profile else population
    rendered = manage.render_config(template.read_text(encoding="utf-8"), settings, runtime_population)
    target = Path("/tmp/world-schema-setup.conf")
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.chmod(target, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(rendered)
    settlement_population.ensure_world_offline()
    result = subprocess.run([str(binary), "--update-databases-only", "-c", str(target)],
                            capture_output=True, text=True, check=False)
    require(result.returncode == 0, "Offline world/characters schema setup failed; inspect private server logs")
    settlement_population.ensure_world_offline()
    demote_retired_pilots(settings)
    print("World and characters schemas updated offline; retired Elwynn pilot control disabled and world simulation was not started.")


def starts_snapshot(settings: manage.Settings) -> list[dict]:
    raw = manage.lab_mysql(settings, """
SELECT JSON_OBJECT('race',race,'class',class,'map',map,'zone',zone,'x',position_x,
 'y',position_y,'z',position_z,'orientation',orientation)
FROM world.playercreateinfo ORDER BY race,class;
""")
    try:
        rows = [json.loads(line) for line in raw.splitlines() if line.strip()]
    except (TypeError, ValueError) as exc:
        raise manage.SetupError("Invalid playercreateinfo SQL read-back") from exc
    require(0 < len(rows) <= 200, "Expected populated playercreateinfo from a compatible TDB import")
    identities = set()
    for row in rows:
        try:
            require(isinstance(row, dict), "Invalid playercreateinfo SQL row")
            identity = (row["race"], row["class"])
            require(all(type(value) is int and 0 < value <= 32 for value in identity) and identity not in identities,
                    "Invalid race/class identities in playercreateinfo")
            require(all(math.isfinite(float(row[key])) for key in ("x", "y", "z", "orientation")),
                    "Invalid coordinates in playercreateinfo")
            identities.add(identity)
        except (KeyError, TypeError, ValueError) as exc:
            if isinstance(exc, manage.SetupError):
                raise
            raise manage.SetupError("Incomplete playercreateinfo snapshot") from exc
    return rows


def start_sql(start: WorldStart, rows: list[dict]) -> str:
    identities = ",".join(f"({row['race']},{row['class']})" for row in rows)
    return f"""START TRANSACTION;
UPDATE world.playercreateinfo SET map={start.map_id}, zone={start.area_id},
 position_x={start.x:.15g}, position_y={start.y:.15g}, position_z={start.z:.15g}, orientation={start.orientation:.15g}
WHERE (race,class) IN ({identities});
COMMIT;
"""


def verify_starts(before: list[dict], after: list[dict], start: WorldStart) -> None:
    require({(row["race"], row["class"]) for row in before} == {(row["race"], row["class"]) for row in after},
            "Player race/class identities changed during starting-location setup")
    for row in after:
        require(row["map"] == start.map_id and row["zone"] == start.area_id and
                all(abs(float(row[key]) - getattr(start, key)) < 0.0001 for key in ("x", "y", "z", "orientation")),
                "Starting-location read-back differs from the reviewed map entry")


def apply_start(data: Path, receipt: Path, settings: manage.Settings) -> None:
    validate_profile(settings)
    settlement_population.ensure_world_offline()
    population = settlement_population.verify_data(data)
    start = WorldStart.load(population)
    before = starts_snapshot(settings)
    # The before-image must exist before any mutation, including a repeated run.
    receipt.parent.mkdir(parents=True, exist_ok=True)
    payload = {"schema_version": 1, "status": "before_mutation", "realm_id": 2,
               "source_revision": "preview-v8", "start": start.__dict__, "before": before,
               "existing_characters_modified": False}
    descriptor = os.open(receipt, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    settlement_population.ensure_world_offline()
    manage.lab_mysql(settings, start_sql(start, before))
    after = starts_snapshot(settings)
    verify_starts(before, after, start)
    payload.update(status="verified", after=after)
    # This path was created exclusively by this invocation and contains no secrets.
    with receipt.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(f"New-character start verified for {len(after)} race/class combinations on map 725; existing characters preserved.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("register")
    schema = sub.add_parser("prepare")
    schema.add_argument("data", type=Path)
    schema.add_argument("template", type=Path)
    schema.add_argument("binary", type=Path)
    start = sub.add_parser("apply-start")
    start.add_argument("data", type=Path)
    start.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    try:
        settings = manage.Settings.load(os.environ)
        if args.command == "register":
            register_world(settings)
        elif args.command == "prepare":
            prepare(args.data, args.template, args.binary, settings)
        else:
            apply_start(args.data, args.receipt, settings)
    except (manage.SetupError, settlement_population.ProfileError, OSError) as exc:
        message = str(exc) if isinstance(exc, (manage.SetupError, settlement_population.ProfileError)) else "Unable to prepare private setup files or execute the server"
        print(f"World setup failed: {message}", file=sys.stderr)
        raise SystemExit(2)


if __name__ == "__main__":
    main()
