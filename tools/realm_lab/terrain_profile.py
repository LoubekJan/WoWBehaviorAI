"""Experimental terrain-v3 profile; the reviewed flat-v2 profiles stay strict.

Only the existing hundred actors' ground heights may change. Live map, VMAP
model closure and directed navigation proofs are checked before any SQL write.
Native movement acceptance is deliberately separate from this navigation gate.
"""
from __future__ import annotations

from dataclasses import replace
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct

try:
    from . import single_return as base, terrain_geometry
except ImportError:
    import single_return as base
    import terrain_geometry

PROFILE = "hunt-terrain-100"
METADATA = base.METADATA / "terrain-v3"
BASELINE_SHA256 = "ca8dff43ecb68d51505617ad62dd34c1b9708c23db43907ad24a6d6db2d60954"
VMAP_MAGIC = b"VMAP_4.8"


def population_module():
    try:
        from . import hunt_population
    except ImportError:
        import hunt_population
    return hunt_population


def baseline_population():
    document = base.load_json(base.METADATA / "hunt-population-100.json")
    canonical = json.dumps(document, sort_keys=True, separators=(",", ":")).encode("utf-8")
    base.require(hashlib.sha256(canonical).hexdigest() == BASELINE_SHA256,
                 "The deployed flat hundred-actor baseline has changed")
    return population_module().Population.load(base.METADATA, profile="hunt-100")


def load_scope(metadata: Path = METADATA) -> base.Scope:
    points = base.load_json(metadata / "test-points.json")
    reviewed = base.load_json(base.METADATA / "test-points.json")
    original = base.Scope.load()
    base.require((points.get("map_id"), points.get("area_id"), points.get("source_revision")) == (725, 4988, "v3"),
                 "Terrain profile requires the reviewed v3 map 725 / area 4988")
    try:
        for field in ("bounds", "test_region", "tile"):
            base.require(points[field] == reviewed[field], "Terrain profile must preserve the original tile and XY scope")
        home = points["home"]
        base.require(home["x"] == original.home_x and home["y"] == original.home_y,
                     "Terrain profile must preserve the original home XY")
        z = base.finite(home["floor_z"])
        returns = points["return_points"]
        base.require(isinstance(returns, list) and len(returns) == 4,
                     "Terrain profile requires the four original return points")
        for point, old in zip(returns, reviewed["return_points"]):
            base.require(all(point[key] == old[key] for key in ("name", "x", "y")),
                         "Terrain profile must preserve return point identities and XY")
            base.finite(point["floor_z"])
    except (KeyError, TypeError) as exc:
        raise base.ProfileError("Incomplete terrain scope metadata") from exc
    return replace(original, home_z=z)


def load_population(metadata: Path = METADATA):
    hunt = population_module()
    scope, baseline = load_scope(metadata), baseline_population()
    document = base.load_json(metadata / "hunt-population-terrain-100.json")
    base.require((document.get("profile"), document.get("map_id"), document.get("area_id"), document.get("source_revision")) ==
                 (PROFILE, 725, 4988, "v3"), "Terrain population profile/map/source proof mismatch")
    base.require(document.get("respawn_seconds") == baseline.respawn_seconds,
                 "Terrain population must preserve the reviewed respawn interval")
    rows = document.get("agents")
    base.require(isinstance(rows, list) and len(rows) == 100, "Terrain population requires exactly 100 actors")
    originals = {actor.spawn_id: actor for actor in baseline.actors}
    actors = []
    try:
        for row in rows:
            base.require(isinstance(row, dict), "Invalid terrain actor")
            spawn = hunt.integer(row["spawn_id"], 900725, 900824, "spawn ID")
            old, home = originals[spawn], row["home"]
            x, y, z, orientation = (base.finite(home[key]) for key in ("x", "y", "z", "o"))
            base.require((row["entry"], row["role"], row["label"], x, y, orientation) ==
                         (old.entry, old.role, old.label, old.x, old.y, old.orientation),
                         "Terrain may change only ground Z; all 100 identities, roles and XY must be preserved")
            actors.append(replace(old, z=z))
    except (KeyError, TypeError) as exc:
        raise base.ProfileError("Incomplete terrain actor metadata") from exc
    base.require({actor.spawn_id for actor in actors} == set(originals), "Terrain actor IDs are missing or duplicated")
    actors.sort(key=lambda actor: actor.spawn_id)
    base.require(actors[0].z == scope.home_z, "Terrain home differs from the first actor's reviewed ground height")
    return hunt.Population(scope, tuple(actors), baseline.respawn_seconds, PROFILE)


class Reader:
    def __init__(self, data: bytes):
        self.data, self.offset = data, 0

    def take(self, count: int) -> bytes:
        base.require(0 <= count <= len(self.data) - self.offset, "Truncated VMAP model closure")
        result = self.data[self.offset:self.offset + count]
        self.offset += count
        return result

    def unpack(self, fmt: str):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))


def vmap_closure(tree: bytes, tile: bytes) -> tuple[set[str], int]:
    """Read native BIH and ModelSpawn layout from this repository's assembler."""
    reader = Reader(tree)
    base.require(reader.take(8) == VMAP_MAGIC and reader.take(1) == b"\1" and reader.take(4) == b"NODE",
                 "Terrain VMAP must be a tiled map with native VMAP_4.8 layout")
    bounds = reader.unpack("<6f")
    base.require(all(math.isfinite(value) for value in bounds), "Invalid VMAP tree bounds")
    nodes, = reader.unpack("<I")
    base.require(nodes > 0 and nodes % 3 == 0, "Invalid VMAP BIH node count")
    reader.take(nodes * 4)
    objects, = reader.unpack("<I")
    base.require(0 < objects <= 10000, "Invalid VMAP BIH object count")
    reader.take(objects * 4)
    base.require(reader.take(4) == b"GOBJ" and reader.offset == len(tree),
                 "Unexpected global VMAP objects or trailing tree data")
    reader = Reader(tile)
    base.require(reader.take(8) == VMAP_MAGIC, "Invalid VMAP tile magic")
    count, = reader.unpack("<I")
    base.require(count == objects, "VMAP tree and tile object counts differ")
    models, ids, references = set(), set(), set()
    for _ in range(count):
        flags, adt_id, identity = reader.unpack("<IHI")
        base.require(flags & ~7 == 0 and flags & 2 == 0 and flags & 4 != 0,
                     "Unexpected unbounded or global VMAP model spawn")
        coordinates = reader.unpack("<7f")
        bounds = reader.unpack("<6f")
        base.require(all(math.isfinite(value) for value in (*coordinates, *bounds)) and coordinates[6] > 0,
                     "Invalid VMAP model placement")
        length, = reader.unpack("<I")
        base.require(0 < length <= 500, "Invalid VMAP model filename length")
        try:
            name = reader.take(length).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise base.ProfileError("Invalid VMAP model filename") from exc
        base.require(re.fullmatch(r"[A-Za-z0-9_. -]+", name) is not None and name not in (".", ".."),
                     "VMAP model filename escapes the model directory")
        reference, = reader.unpack("<I")
        base.require(identity not in ids and reference < objects and reference not in references,
                     "Duplicate or invalid VMAP model/tree identity")
        ids.add(identity)
        references.add(reference)
        models.add(f"vmaps/{name}.vmo")
    base.require(reader.offset == len(tile), "Trailing VMAP tile bytes")
    return models, count


def required_routes(population, points: dict) -> set[tuple[str, str]]:
    home = str(base.SPAWN_ID)
    result = {(home, point["name"]) for point in points["return_points"]}
    result |= {(to, start) for start, to in tuple(result)}
    prey = [actor for actor in population.actors if actor.role == "prey"]
    for actor in population.actors:
        if actor.role != "predator":
            continue
        nearby = sorted(prey, key=lambda target: (math.hypot(actor.x - target.x, actor.y - target.y), target.spawn_id))[:2]
        base.require(len(nearby) == 2 and all(math.hypot(actor.x - target.x, actor.y - target.y) <= 25 for target in nearby),
                     "Terrain predators require two nearby prey homes")
        for target in nearby:
            start, to = str(actor.spawn_id), str(target.spawn_id)
            result.update(((start, to), (to, start)))
    return result


def verify_navigation(nav: dict, population, points: dict) -> None:
    routes = required_routes(population, points)
    base.require(nav.get("ok") is True and nav.get("scope") == "navmesh_only" and nav.get("map_id") == 725 and
                 (nav.get("projected_points"), nav.get("actor_points"), nav.get("complete_paths"), nav.get("required_paths")) ==
                 (104, 100, len(routes), len(routes)), "Complete terrain home/hunt/return navigation proof required")
    expected = {str(actor.spawn_id): (actor.x, actor.y, actor.z) for actor in population.actors}
    expected.update({point["name"]: (point["x"], point["y"], point["floor_z"]) for point in points["return_points"]})
    projections = nav.get("projections")
    base.require(isinstance(projections, list) and len(projections) == 104, "All terrain point projections are required")
    seen = set()
    for point in projections:
        key = str(point["spawn_id"]) if "spawn_id" in point else point.get("name")
        base.require(key in expected and key not in seen, "Unknown or duplicate terrain point projection")
        values = tuple(base.finite(point[field]) for field in ("x", "y", "z"))
        projected = tuple(base.finite(point[field]) for field in ("projected_x", "projected_y", "projected_z"))
        base.require(all(base.near(actual, reviewed) for actual, reviewed in zip(values, expected[key])) and
                     math.hypot(projected[0] - values[0], projected[1] - values[1]) <= 1 and
                     abs(projected[2] - values[2]) <= 2, "Terrain projection differs from reviewed home or ground")
        seen.add(key)
    paths = nav.get("paths")
    base.require(isinstance(paths, list) and len(paths) == len(routes) and
                 {(path.get("from"), path.get("to")) for path in paths} == routes and
                 all(path.get("complete") is True for path in paths), "Terrain directed routes are incomplete or differ from reviewed pairs")


def verify_data(data: Path, metadata: Path = METADATA):
    try:
        return _verify_data(data, metadata)
    except (KeyError, TypeError, AttributeError) as exc:
        raise base.ProfileError("Incomplete terrain bundle proof") from exc


def _verify_data(data: Path, metadata: Path):
    population = load_population(metadata)
    project = base.load_json(metadata / "project-manifest.json")
    server = base.load_json(metadata / "server-manifest.json")
    points = base.load_json(metadata / "test-points.json")
    for document in (project, server, points):
        base.require((document.get("map_id"), document.get("area_id"), document.get("source_revision")) == (725, 4988, "v3"),
                     "Terrain map/area/source proof mismatch")
    patch = project.get("client", {}).get("patch", {}).get("sha256")
    base.require(isinstance(patch, str) and re.fullmatch(r"[a-f0-9]{64}", patch) is not None and
                 server.get("client_patch_sha256") == patch, "Terrain server/client patch proof mismatch")
    verify_navigation(server.get("navigation", {}), population, points)
    root, contents = data.resolve(), {}
    files = server.get("files")
    base.require(isinstance(files, list) and len(files) >= 9, "Terrain map and complete VMAP model hashes required")
    for item in files:
        name = item["path"]
        base.require(isinstance(name, str) and not PurePosixPath(name).is_absolute() and
                     str(PurePosixPath(name)) == name and ".." not in PurePosixPath(name).parts and "\\" not in name,
                     "Terrain file path escapes lab DataDir")
        path = (root / name).resolve()
        base.require(path.is_relative_to(root) and name not in contents, "Duplicate or escaping terrain file")
        try:
            content = path.read_bytes()
        except OSError as exc:
            raise base.ProfileError(f"Missing live terrain file: {name}") from exc
        base.require(len(content) == item.get("bytes") and hashlib.sha256(content).hexdigest() == item.get("sha256"),
                     f"Live terrain file differs from reviewed bundle: {name}")
        contents[name] = content
    tree_name, tile_name = "vmaps/725.vmtree", "vmaps/725_30_31.vmtile"
    base.require(tree_name in contents and tile_name in contents, "Terrain VMAP tree and tile required")
    models, objects = vmap_closure(contents[tree_name], contents[tile_name])
    expected = {"dbc/Map.dbc", "dbc/AreaTable.dbc", "dbc/Light.dbc", "maps/7253130.map", "mmaps/725.mmap", "mmaps/7253130.mmtile",
                tree_name, tile_name} | models
    base.require(set(contents) == expected, "Terrain manifest must include the complete VMAP model closure")
    try:
        ground = terrain_geometry.read_float_map(contents["maps/7253130.map"])
        samples = [(str(actor.spawn_id), actor.x, actor.y, actor.z) for actor in population.actors]
        samples.extend((point["name"], point["x"], point["y"], point["floor_z"]) for point in points["return_points"])
        for name, x, y, reviewed_z in samples:
            actual_z = terrain_geometry.gridmap_height(ground, x, y)
            base.require(abs(actual_z - reviewed_z) <= 0.001,
                         f"Terrain reviewed ground height differs from live extracted map: {name}")
    except ValueError as exc:
        if isinstance(exc, base.ProfileError):
            raise
        raise base.ProfileError(f"Invalid live extracted terrain ground: {exc}") from exc
    base.require(server.get("custom_vmaps", {}).get("status") == "verified_present" and
                 server["custom_vmaps"].get("objects") == objects and
                 project.get("geometry", {}).get("model_placements") == 4 and
                 project.get("geometry", {}).get("vmap_instances") == objects,
                 "Terrain object placement / VMAP closure proof mismatch")
    for model in models:
        base.require(contents[model].startswith(VMAP_MAGIC + b"WMOD"), "Invalid terrain VMAP model header")
    base.require({path.name for path in (root / "maps").glob("725*.map")} == {"7253130.map"} and
                 {path.name for path in (root / "mmaps").glob("725*")} == {"725.mmap", "7253130.mmtile"} and
                 {path.name for path in (root / "vmaps").glob("725*.vmtree")} == {"725.vmtree"} and
                 {path.name for path in (root / "vmaps").glob("725*.vmtile")} == {"725_30_31.vmtile"},
                 "Unexpected additional terrain/map/navigation/VMAP tile")
    rows, strings = base.read_dbc(contents["dbc/Map.dbc"], 66)
    selected = [row for row in rows if row[0] == 725]
    base.require(len(selected) == 1, "Terrain Map.dbc binding missing")
    row = selected[0]
    base.require(row[1] < len(strings) and strings[row[1]:].split(b"\0", 1)[0] == b"AIWorldLab" and
                 (row[2], row[22], row[63]) == (0, 4988, 2), "Wrong terrain Map.dbc binding")
    rows, _ = base.read_dbc(contents["dbc/AreaTable.dbc"], 36)
    selected = [row for row in rows if row[0] == 4988]
    base.require(len(selected) == 1 and selected[0][1:3] == (725, 0) and selected[0][3] < 4096 and
                 sum(row[3] == selected[0][3] for row in rows) == 1, "Wrong terrain AreaTable binding")
    return population


def validate_transition_snapshot(rows: list[dict], population) -> str:
    hunt = population_module()
    for label, expected in (("flat-v2", baseline_population()), ("terrain-v3", population)):
        try:
            hunt.validate_snapshot(rows, expected, control=None)
            return label
        except base.ProfileError:
            continue
    raise base.ProfileError("Terrain transition requires the complete flat-v2 or terrain-v3 database state; drift/mixed states are forbidden")


def transition_sql(population) -> str:
    """Atomic v2-to-v3 ground update; all 100 old or all 100 new bindings only."""
    hunt = population_module()
    baseline = baseline_population()
    ids = population.spawn_ids
    def ready(expected):
        bindings = []
        for actor in expected.actors:
            bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(c.position_x-{actor.x})<=0.001 AND ABS(c.position_y-{actor.y})<=0.001
 AND ABS(c.position_z-{actor.z})<=0.001 AND ABS(c.orientation-{actor.orientation})<=0.001
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
        return f"""(SELECT COUNT(*)=100 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 INNER JOIN world.ai_agent_type_entry_defaults t ON t.creature_entry=c.id
 LEFT JOIN characters.ai_agent_group_members g ON g.member_agent_id=a.agent_id
 WHERE a.map_id=725 AND a.control_mode IN (0,1) AND a.home_map_id=725
 AND a.work_map_id IS NULL AND a.world_faction_id=0 AND p.participation_mode=0 AND t.agent_type=a.agent_type
 AND g.member_agent_id IS NULL AND c.zoneId=4988 AND c.areaId=4988
 AND c.spawnMask=1 AND c.phaseMask=1 AND c.MovementType=0 AND c.wander_distance=0
 AND c.spawntimesecs={population.respawn_seconds} AND c.ScriptName=''
 AND c.npcflag=0 AND c.unit_flags=0 AND c.dynamicflags=0
 AND ct.AIName='' AND ct.ScriptName='' AND ct.RegenHealth=1 AND ct.VehicleId=0 AND ct.npcflag=0
 AND (ct.unit_flags & {hunt.FORBIDDEN_TEMPLATE_FLAGS})=0 AND ct.faction>0
 AND ({' OR '.join(bindings)}))"""
    creature_cases = " ".join(f"WHEN {actor.spawn_id} THEN {actor.z}" for actor in population.actors)
    return f"""START TRANSACTION;
SET @lab_terrain_ready = ({ready(baseline)} OR {ready(population)})
 AND (SELECT COUNT(*) FROM world.creature WHERE map=725)=100
 AND (SELECT COUNT(*) FROM characters.ai_agents WHERE map_id=725)=100
 AND NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE control_mode=1 AND agent_id NOT IN ({ids}))
 AND NOT EXISTS(SELECT 1 FROM world.creature
 WHERE (StringId='{base.SPAWN_LABEL}' OR StringId LIKE 'aiworld_lab_hunt_%') AND guid NOT IN ({ids}));
UPDATE world.creature SET position_z=CASE guid {creature_cases} ELSE position_z END
 WHERE @lab_terrain_ready=1 AND map=725 AND guid IN ({ids});
UPDATE characters.ai_agents SET home_z=CASE agent_id {creature_cases} ELSE home_z END, control_mode=0
 WHERE @lab_terrain_ready=1 AND map_id=725 AND agent_id=spawn_id AND agent_id IN ({ids});
COMMIT;
"""
