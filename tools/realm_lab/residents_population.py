"""Two independently named native residents in each reviewed v10 farmhouse.

The population is separate from the retired v8 simulation. Only absent owned
spawn IDs are inserted, offline; existing homes and unrelated data are never
rewritten. Character-template ownership is verified by character_templates.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path

try:
    from . import map_bundle, settlement_population as settlement, single_return as base
except ImportError:
    import map_bundle
    import settlement_population as settlement
    import single_return as base

PROFILE = "residents-v10"
SOURCE_REVISION = "preview-v10"
METADATA = map_bundle.METADATA / SOURCE_REVISION
MANIFEST = "residents-population.json"
POSITION_PROOF = "residents-position-verification.json"
HOUSE_IDS = (178, 179, 183, 184, 193, 194, 196, 197, 213, 214, 215)
SPAWN_IDS = frozenset(range(902000, 902022))
ENTRY_BASE = 9810000
BASE_ENTRY = 11328
BASE_TEMPLATE = dict(entry=11328, ai_name="", script_name="", regenerate_health=1,
                     vehicle=0, npcflag=0, unit_flags=0, unit_flags2=2048,
                     faction=12, minlevel=6, maxlevel=7, flags_extra=2)
require, ProfileError = base.require, base.ProfileError


def geometry_digest(files) -> str:
    return hashlib.sha256(json.dumps(list(files), sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode("utf-8")).hexdigest()


def expected_entry(spawn_id: int) -> int:
    require(type(spawn_id) is int and spawn_id in SPAWN_IDS, "Unknown resident identity")
    return ENTRY_BASE + spawn_id - min(SPAWN_IDS)


@dataclass(frozen=True)
class Actor:
    spawn_id: int
    entry: int
    house_id: int
    slot: int
    x: float
    y: float
    z: float
    orientation: float
    label: str
    faction: str = "stormwind"
    role: str = "resident"


@dataclass(frozen=True)
class Population:
    actors: tuple[Actor, ...]
    geometry: map_bundle.GeometryBundle
    respawn_seconds: int = 300
    profile: str = PROFILE

    @property
    def settlement(self): return self

    @property
    def geometry_files(self): return self.geometry.geometry_files

    @property
    def source_revision(self): return self.geometry.source_revision

    @property
    def metadata(self): return self.geometry.metadata

    @property
    def entry_proof(self): return self.geometry.entry_proof

    @property
    def entry_proof_sha256(self): return self.geometry.entry_proof_sha256

    @property
    def spawn_ids(self): return ",".join(str(actor.spawn_id) for actor in self.actors)

    @staticmethod
    def expected_entry(spawn_id): return expected_entry(spawn_id)

    @classmethod
    def load(cls, metadata: Path = METADATA, geometry=None):
        try:
            geometry = geometry or map_bundle.GeometryBundle.load(metadata.parent)
            require(geometry.source_revision == SOURCE_REVISION and geometry.metadata == metadata,
                    "Residents require the pinned preview-v10 geometry")
            document = base.load_json(metadata / MANIFEST)
            require((document.get("schema_version"), document.get("profile"), document.get("realm_id"),
                     document.get("map_id"), document.get("area_id"), document.get("source_revision")) ==
                    (1, PROFILE, 2, 725, 4988, SOURCE_REVISION), "Wrong resident population identity")
            require(document.get("geometry_sha256") == geometry_digest(geometry.geometry_files),
                    "Resident homes belong to different map geometry")
            require(document.get("templates") == [BASE_TEMPLATE] and
                    type(document.get("respawn_seconds")) is int and 30 <= document["respawn_seconds"] <= 3600,
                    "Resident native template or respawn contract changed")
            proof_path = metadata / POSITION_PROOF
            require(hashlib.sha256(proof_path.read_bytes()).hexdigest() == document.get("position_proof_sha256"),
                    "Resident indoor-position proof hash mismatch")
            proof = base.load_json(proof_path)
            require(proof.get("profile") == PROFILE and proof.get("source_revision") == SOURCE_REVISION and
                    proof.get("map_id") == 725 and proof.get("ok") is True and
                    proof.get("geometry_sha256") == geometry_digest(geometry.geometry_files) and
                    proof.get("geometry_files") == list(geometry.geometry_files) and
                    isinstance(proof.get("native_navigation"), dict) and proof["native_navigation"].get("ok") is True,
                    "Resident indoor geometry proof failed")
            native = proof["native_navigation"]
            require(all(native.get(key) == expected for key, expected in
                        (("map_id", 725), ("poly_ref_bits", 64), ("tiles", 4), ("include_flags", 1),
                         ("exclude_flags", 65534), ("valid_projections", 23), ("required_paths", 44), ("complete_paths", 44))),
                    "Resident navigation must verify 22 homes and 44 complete bidirectional ground paths")
            buildings = proof.get("buildings")
            require(isinstance(buildings, list) and len(buildings) == len(HOUSE_IDS) and
                    {row.get("house_id") for row in buildings} == set(HOUSE_IDS),
                    "Resident proof must cover exactly the eleven residential houses")
            positions = {}
            native_files = {row["path"]: row["sha256"] for row in geometry.geometry_files}
            for building in buildings:
                require(native_files.get("vmaps/" + str(building.get("mesh_file"))) == building.get("mesh_sha256") and
                        isinstance(building.get("mesh_sha256"), str),
                        "Resident building mesh is outside the pinned native geometry")
                points = building.get("positions")
                require(isinstance(points, list) and len(points) == 2 and
                        {p.get("slot") for p in points} == {1, 2}, "Each house requires two distinct resident slots")
                for point in points:
                    key = (building["house_id"], point["slot"])
                    require(key not in positions, "Duplicate resident proof position")
                    positions[key] = point
            rows = document.get("actors")
            require(isinstance(rows, list) and len(rows) == len(SPAWN_IDS), "Exactly 22 resident homes are required")
            actors, seen = [], set()
            for index, row in enumerate(rows):
                require(isinstance(row, dict), "Invalid resident actor")
                spawn, house, slot = min(SPAWN_IDS) + index, HOUSE_IDS[index // 2], index % 2 + 1
                require((row.get("spawn_id"), row.get("entry"), row.get("house_id"), row.get("slot"), row.get("label")) ==
                        (spawn, BASE_ENTRY, house, slot, f"aiworld_lab_resident_{spawn}") and
                        all(type(row.get(key)) is int for key in ("spawn_id", "entry", "house_id", "slot")),
                        "Resident identity, house assignment or label changed")
                coordinates = [base.finite(row.get(key)) for key in ("x", "y", "z", "orientation")]
                require(0 <= coordinates[3] < math.tau and
                        settlement.ACTIVE_BOUNDS["min_x"] <= coordinates[0] <= settlement.ACTIVE_BOUNDS["max_x"] and
                        settlement.ACTIVE_BOUNDS["min_y"] <= coordinates[1] <= settlement.ACTIVE_BOUNDS["max_y"],
                        "Resident home is outside the active map or has invalid orientation")
                point = positions[(house, slot)]
                require(all(base.finite(point.get(key)) == value for key, value in zip(("x", "y", "z", "orientation"), coordinates)),
                        "Resident home differs from its reviewed indoor point")
                floor, navigation = point.get("geometry"), point.get("navigation")
                require(isinstance(floor, dict) and isinstance(navigation, dict) and navigation.get("valid") is True and
                        all(floor.get(key) is True for key in ("dry", "indoor", "full_native_scene_cylinder_clear",
                                                               "source_all_water_layers_checked")) and
                        type(floor.get("group_flags")) is int and floor["group_flags"] & 0x2000 and
                        0 <= coordinates[2] - base.finite(floor.get("floor_z")) <= .15 and
                        0 <= base.finite(floor.get("slope_degrees")) <= 15 and
                        base.finite(floor.get("roof_clearance")) >= 2.3 and
                        base.finite(floor.get("clear_radius")) >= .75 and
                        0 <= base.finite(floor.get("floor_circle_height_range")) <= .5,
                        "Resident point lacks clear, level indoor floor and native navigation evidence")
                projection = navigation.get("projection", {})
                require(isinstance(projection, dict) and projection.get("valid") is True and projection.get("flags") == 1 and
                        0 <= base.finite(projection.get("xy_delta")) <= 1 and
                        0 <= base.finite(projection.get("z_delta")) <= 2,
                        "Resident point has no nearby ground-only navigation projection")
                for key in ("entry_path", "return_path"):
                    route = navigation.get(key, {})
                    require(isinstance(route, dict) and route.get("complete") is True and
                            route.get("all_corridor_flags_ground_only") is True and
                            isinstance(route.get("status"), dict) and route["status"].get("succeeded") is True and
                            all(route["status"].get(flag) is False for flag in
                                ("partial", "buffer_too_small", "out_of_nodes", "failed")),
                            "Resident entry/return path is incomplete or leaves ground navigation")
                signature = tuple(coordinates[:3])
                require(signature not in seen, "Residents must not occupy the same standing point")
                seen.add(signature)
                actors.append(Actor(spawn, BASE_ENTRY, house, slot, *coordinates, row["label"]))
            return cls(tuple(actors), geometry, document["respawn_seconds"])
        except (KeyError, TypeError, ValueError, OSError) as exc:
            if isinstance(exc, ProfileError): raise
            raise ProfileError("Missing or invalid resident population/position proof") from exc

    def config(self):
        bounds = settlement.ACTIVE_BOUNDS
        return {"AIWorld.ScopeMapId": "725", "AIWorld.ScopeZoneIds": '"4988"',
                "AIWorld.ScopeBoundsEnabled": "1",
                **{f"AIWorld.Scope{key.title().replace('_', '')}": str(value) for key, value in bounds.items()},
                "AIWorld.ScopeSpawnIds": f'"{self.spawn_ids}"',
                "AIWorld.ScopeRestrictAgents": "1", "AIWorld.ScopeAlwaysActive": "1"}

    def observer_environment(self):
        return {"WORLD_VIEWER_SCOPE_MAP_ID": "725", "WORLD_VIEWER_SCOPE_ZONE_IDS": "4988",
                "WORLD_VIEWER_SCOPE_NAME": "AI World Lab", "WORLD_VIEWER_SCOPE_BOUNDS": json.dumps(settlement.BOUNDS, separators=(",", ":"))}


def verify_data(data: Path, metadata: Path = METADATA):
    geometry = map_bundle.verify_data(data, metadata.parent)
    return Population.load(metadata, geometry)


def snapshot_sql(population: Population):
    reserved = lambda field: f"({field} BETWEEN 900725 AND 900824 OR {field} BETWEEN 901000 AND 901099 OR {field} BETWEEN 902000 AND 902021)"
    return f"""
SELECT JSON_OBJECT('kind','template','entry',entry,'ai_name',AIName,'script_name',ScriptName,
 'regenerate_health',RegenHealth,'vehicle',VehicleId,'npcflag',npcflag,'unit_flags',unit_flags,
 'unit_flags2',unit_flags2,'faction',faction,'minlevel',minlevel,'maxlevel',maxlevel,'flags_extra',flags_extra)
 FROM world.creature_template WHERE entry={BASE_ENTRY};
SELECT JSON_OBJECT('kind','spawn','guid',guid,'entry',id,'equipment_id',equipment_id,'map',map,'zone',zoneId,'area',areaId,
 'spawn_mask',spawnMask,'phase_mask',phaseMask,'x',position_x,'y',position_y,'z',position_z,
 'orientation',orientation,'movement',MovementType,'wander',wander_distance,'label',StringId,
 'script_name',ScriptName,'respawn',spawntimesecs,'npcflag',npcflag,'unit_flags',unit_flags,'dynamicflags',dynamicflags)
 FROM world.creature WHERE map=725 OR {reserved('guid')} OR StringId LIKE 'aiworld_lab_resident_%';
SELECT JSON_OBJECT('kind','agent','id',agent_id) FROM characters.ai_agents
 WHERE map_id=725 OR {reserved('agent_id')} OR {reserved('spawn_id')} OR control_mode=1;
SELECT JSON_OBJECT('kind','participation','spawn',spawn_id) FROM world.ai_spawn_participation_defaults WHERE {reserved('spawn_id')};
SELECT JSON_OBJECT('kind','membership','member',member_agent_id) FROM characters.ai_agent_group_members WHERE {reserved('member_agent_id')};
SELECT JSON_OBJECT('kind','memory','id',agent_id) FROM characters.ai_long_term_memories WHERE {reserved('agent_id')};
SELECT JSON_OBJECT('kind','smart_script','id',id) FROM world.smart_scripts
 WHERE source_type=0 AND (entryorguid={BASE_ENTRY} OR entryorguid BETWEEN -902021 AND -902000);
SELECT JSON_OBJECT('kind','spawn_addon','guid',guid) FROM world.creature_addon WHERE guid IN ({population.spawn_ids});
SELECT JSON_OBJECT('kind','moving_template','entry',entry) FROM world.creature_template_addon
 WHERE entry={BASE_ENTRY} AND (path_id<>0 OR mount<>0 OR StandState<>0);
"""


def validate_snapshot(rows, population: Population, *, bootstrap=False):
    require(isinstance(rows, list) and all(isinstance(row, dict) for row in rows), "Invalid resident database read-back")
    require(all(row.get("kind") in ("template", "spawn", "agent", "participation", "membership", "memory",
                                   "smart_script", "spawn_addon", "moving_template") for row in rows),
            "Unknown resident database row")
    templates = [row for row in rows if row.get("kind") == "template"]
    require(len(templates) == 1 and templates[0] == {"kind": "template", **BASE_TEMPLATE},
            "Resident native source template changed")
    require(all(row["kind"] in ("template", "spawn") for row in rows),
            "Resident world must contain no retired/controlled AI state, scripts or movement overrides")
    actors, seen = {actor.spawn_id: actor for actor in population.actors}, set()
    for row in rows:
        if row["kind"] != "spawn": continue
        identity = row.get("guid")
        require(type(identity) is int and identity in actors and identity not in seen, "Foreign or duplicate resident spawn")
        actor = actors[identity]
        require(all(row.get(field) == expected for field, expected in (
            ("entry", actor.entry), ("equipment_id", -1), ("map", 725), ("zone", 4988), ("area", 4988),
            ("spawn_mask", 1), ("phase_mask", 1), ("movement", 0), ("wander", 0),
            ("label", actor.label), ("script_name", ""), ("respawn", population.respawn_seconds),
            ("npcflag", 0), ("unit_flags", 0), ("dynamicflags", 0))) and
            all(base.near(row.get(field), value) for field, value in
                (("x", actor.x), ("y", actor.y), ("z", actor.z), ("orientation", actor.orientation))),
            "Resident immutable home, template binding or stationary spawn changed")
        seen.add(identity)
    require(bootstrap or seen == SPAWN_IDS, "Resident population is incomplete")


def bootstrap_sql(population: Population):
    # No UPDATE/DELETE and no broad INSERT IGNORE: foreign collisions fail the
    # preceding exact read-back and every newly inserted home is verified again.
    statements = ["START TRANSACTION;"]
    for actor in population.actors:
        statements.append(f"""INSERT INTO world.creature (guid,id,equipment_id,map,zoneId,areaId,spawnMask,phaseMask,
 position_x,position_y,position_z,orientation,spawntimesecs,wander_distance,MovementType,curhealth,curmana,StringId,VerifiedBuild)
 SELECT {actor.spawn_id},{actor.entry},-1,725,4988,4988,1,1,
 {actor.x},{actor.y},{actor.z},{actor.orientation},{population.respawn_seconds},0,0,0,0,'{actor.label}',12340
 WHERE NOT EXISTS(SELECT 1 FROM world.creature WHERE guid={actor.spawn_id});""")
    return "\n".join([*statements, "COMMIT;\n"])


def validate_telemetry(state, population: Population):
    require(isinstance(state, dict) and state.get("version") == 4 and state.get("stale") is False and
            type(state.get("captured_at_ms")) is int and state["captured_at_ms"] > 0,
            "Residents require a fresh native telemetry snapshot")
    actors = state.get("agents")
    require(isinstance(actors, list) and len(actors) == len(SPAWN_IDS) and
            all(isinstance(row, dict) and type(row.get("spawn_id")) is int for row in actors) and
            {row["spawn_id"] for row in actors} == SPAWN_IDS, "Telemetry must contain exactly the 22 residents")
    homes = {actor.spawn_id: actor for actor in population.actors}
    for row in actors:
        home = homes[row["spawn_id"]]
        position = row.get("position")
        require(row.get("native_creature") is True and row.get("control_mode") == "OBSERVE_ONLY" and
                row.get("agent_id") == home.spawn_id and isinstance(position, dict) and position.get("map_id") == 725 and
                all(type(position.get(key)) in (int, float) and math.isfinite(position[key]) and
                    abs(position[key] - getattr(home, key)) <= .5 for key in ("x", "y", "z")) and
                all(row.get(key) is None for key in ("needs", "economy", "living_role", "supply", "raid")),
                "Resident telemetry acquired simulation ownership or moved away from its reviewed home")
    return {"total": 22, "managed": 0, "native": 22, "live_roles": 0}
