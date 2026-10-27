"""Twelve reviewed native farmers over the unchanged 28-actor raid profile.

Only AI identity reservations persist. Crops, cargo and warehouse stock belong
to the runtime food ledger and start afresh after every server restart.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path

try:
    from . import raid_population as raids, settlement_population as settlement, single_return as base
except ImportError:
    import raid_population as raids
    import settlement_population as settlement
    import single_return as base

PROFILE = "fields-v8"
METADATA = settlement.METADATA
MANIFEST = "field-population.json"
NAV_PROOF = "field-nav-probe.json"
TEXTURE_AUDIT = "field-texture-audit.json"
ASSIGNMENT_PROPOSAL = "field-work-assignment-proposal.json"
BASE_RAIDS_SHA256 = "df27cb62d694b39263cea6d9dd9e1781974d6afdf3b5613a78f9268aa2e7df8b"
TEXTURE_AUDIT_SHA256 = "b0b18a1d8e354c11029b8d8432c99b77c4d0eb09d946c598175abc26b15eb7fe"
ASSIGNMENT_PROPOSAL_SHA256 = "af083a8b7d37d680ca122cc05fed5eea9925185a537c95a575ba125d05528c75"
FIELD_WORKERS = {4: tuple(range(901016, 901020)), 5: tuple(range(901022, 901026)),
                 6: tuple(range(901028, 901032))}
FIELD_VILLAGES = {4: "villageTop", 5: "villageLeft", 6: "villageRight"}
RECRUIT_IDS = frozenset(identity for workers in FIELD_WORKERS.values() for identity in workers)
SPAWN_IDS = OWNED_IDS = raids.SPAWN_IDS | RECRUIT_IDS
NATIVE_IDS = settlement.SPAWN_IDS - SPAWN_IDS
WORK_FIELDS = raids.consumers.supply.wildlife.WORK_FIELDS
FIELDS = {**raids.FIELDS, "field_work": WORK_FIELDS}
ProfileError, require, parse_snapshot = base.ProfileError, base.require, base.parse_snapshot


@dataclass(frozen=True)
class Recruit:
    spawn_id: int
    entry: int
    label: str
    x: float
    y: float
    z: float
    orientation: float
    role: str
    depot_id: int
    worker_ordinal: int
    agent_type: int = 0
    world_faction: int = 1

    @property
    def field_id(self):
        return self.depot_id


def _pinned(metadata: Path, filename: str, expected: str) -> dict:
    try:
        require(hashlib.sha256((metadata / filename).read_bytes()).hexdigest() == expected,
                "Fields require unchanged reviewed " + filename)
        return json.loads((metadata / filename).read_text(encoding="utf-8"))
    except OSError as exc:
        raise ProfileError("Missing reviewed field baseline " + filename) from exc


@dataclass(frozen=True)
class Population:
    raids: raids.Population
    recruits: tuple[Recruit, ...]
    fields: tuple[dict, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        _pinned(metadata, raids.MANIFEST, BASE_RAIDS_SHA256)
        population = raids.Population.load(metadata)
        audit = _pinned(metadata, TEXTURE_AUDIT, TEXTURE_AUDIT_SHA256)
        _pinned(metadata, ASSIGNMENT_PROPOSAL, ASSIGNMENT_PROPOSAL_SHA256)
        document = base.load_json(metadata / MANIFEST)
        require((document.get("schema_version"), document.get("profile"), document.get("realm_id"),
                 document.get("map_id"), document.get("area_id"), document.get("source_revision")) ==
                (1, PROFILE, 2, 725, 4988, "preview-v8"), "Field profile/realm/map/source mismatch")
        require(document.get("raid_manifest_sha256") == BASE_RAIDS_SHA256 and
                document.get("texture_audit_sha256") == TEXTURE_AUDIT_SHA256 and
                document.get("assignment_proposal_sha256") == ASSIGNMENT_PROPOSAL_SHA256,
                "Fields do not pin the reviewed raid, texture and assignment baseline")
        require(document.get("owned_spawn_ids") == sorted(SPAWN_IDS) and
                document.get("native_spawn_ids") == sorted(NATIVE_IDS) and
                document.get("recruit_spawn_ids") == sorted(RECRUIT_IDS) and
                document.get("runtime_food_state_only") is True and
                document.get("all_authored_homes_preserved") is True and
                type(document.get("native_geometry_files_verified")) is int and
                document["native_geometry_files_verified"] == 61 and
                document.get("navigation_proof_file") == NAV_PROOF and
                document.get("work_point_rotation") == "worker_ordinal + 4 * (cycle % 3)" and
                all(type(document.get(key)) is int and document[key] == value for key, value in
                    (("harvest_work_ms", 15000), ("harvest_amount", 1),
                     ("delivery_work_ms", 5000), ("meal_work_ms", 5000))),
                "Field ownership, rotation, timing or restart policy changed")
        actors = {actor.spawn_id: actor for actor in population.settlement.actors}
        expected_fields, expected_recruits, recruits = [], [], []
        for identity, workers in FIELD_WORKERS.items():
            village = FIELD_VILLAGES[identity]
            plot = next(plot for plot in audit["plots"] if plot["nearest_village"] == village)
            source = actors[workers[0]]
            expected_fields.append({"id": identity, "name": plot["id"], "village": village,
                "centroid": {"map_id": 725, **plot["centroid"], "z": 0.0},
                "warehouse": {"id": identity, "name": village + " warehouse", "world_faction_id": 1,
                    "position": {"map_id": 725, "x": source.x, "y": source.y, "z": source.z},
                    "capacity": 30, "target_stock": 12, "low_threshold": 4, "initial_stock": 0,
                    "source_spawn_id": source.spawn_id},
                "work_points": [{"map_id": 725, "x": point["x"], "y": point["y"], "z": point["terrain_z"],
                    "crop_alpha": point["crop_alpha"],
                    "crop_boundary_clearance_yards": point["paint_mask_boundary_clearance_yards"]}
                    for point in plot["candidates"]],
                "crop_capacity": 24, "initial_crop_potential": 24, "regrow_amount": 1, "regrow_interval_ms": 30000})
            for ordinal, spawn in enumerate(workers):
                actor = actors[spawn]
                require(actor.faction == "stormwind" and actor.role == "worker", "Field actor must remain native worker")
                expected_recruits.append({"spawn_id": spawn, "entry": actor.entry, "label": actor.label,
                    "role": actor.role, "capability": "farmer", "living_role": "FARMER", "agent_type": 0,
                    "world_faction_id": 1, "field_id": identity, "depot_id": identity, "worker_ordinal": ordinal,
                    "work_point_indices": [ordinal + 4 * cycle for cycle in range(3)], "cargo_capacity": 2,
                    "home": {"x": actor.x, "y": actor.y, "z": actor.z, "o": actor.orientation}})
                recruits.append(Recruit(spawn, actor.entry, actor.label, actor.x, actor.y, actor.z,
                    actor.orientation, actor.role, identity, ordinal))
        # Canonical JSON comparison rejects bool/int substitutions as well as
        # moved anchors, foreign workers and changed alpha candidates.
        require(settlement.digest(document.get("fields")) == settlement.digest(expected_fields) and
                settlement.digest(document.get("recruits")) == settlement.digest(sorted(expected_recruits, key=lambda r: r["spawn_id"])),
                "Field anchors, crop capacity, workers, immutable homes or painted work points changed")
        return cls(population, tuple(sorted(recruits, key=lambda actor: actor.spawn_id)), tuple(expected_fields))

    @property
    def settlement(self): return self.raids.settlement
    @property
    def wildlife(self): return self.raids.wildlife
    @property
    def supply(self): return self.raids.supply
    @property
    def consumers(self): return self.raids.consumers
    @property
    def actors(self): return self.raids.actors + self.recruits
    @property
    def spawn_ids(self): return ",".join(str(identity) for identity in sorted(SPAWN_IDS))
    @property
    def geometry_files(self): return self.settlement.geometry_files
    @property
    def respawn_seconds(self): return self.settlement.respawn_seconds

    def config(self):
        return {**self.raids.config(), "AIWorld.LivingFieldsEnabled": "1"}

    def observer_environment(self):
        return self.settlement.observer_environment()


def navigation_requirements(population: Population) -> tuple[dict[str, dict], set[tuple[str, str]]]:
    anchors, routes = {}, set()
    for actor in population.recruits:
        home, depot = f"home_{actor.spawn_id}", f"warehouse_{actor.depot_id}"
        anchors[home] = {"x": actor.x, "y": actor.y, "z": actor.z}
        routes.update(((home, depot), (depot, home)))
    for field in population.fields:
        identity = field["id"]
        depot = f"warehouse_{identity}"
        anchors[depot] = {key: field["warehouse"]["position"][key] for key in ("x", "y", "z")}
        for index, point in enumerate(field["work_points"]):
            name = f"field_{identity}_point_{index}"
            anchors[name] = {key: point[key] for key in ("x", "y", "z")}
            routes.update(((depot, name), (name, depot)))
        for ordinal in range(4):
            points = [f"field_{identity}_point_{ordinal + 4 * cycle}" for cycle in range(3)]
            routes.update(zip(points, points[1:] + points[:1]))
    return anchors, routes


def verify_navigation(population: Population, metadata: Path = METADATA) -> None:
    proof = base.load_json(metadata / NAV_PROOF)
    require((proof.get("schema_version"), proof.get("profile"), proof.get("source_revision"), proof.get("map_id")) ==
            (1, PROFILE, "preview-v8", 725) and proof.get("status") == "PASS" and proof.get("ok") is True and
            proof.get("scope") == "native_detour_ground_connectivity_only" and
            proof.get("physical_movement_verified") is False and proof.get("native_collision_queries_verified") is False and
            proof.get("native_geometry_files_verified") == 61 and
            (proof.get("population_manifest") or {}).get("sha256") == hashlib.sha256((metadata / MANIFEST).read_bytes()).hexdigest(),
            "Missing or mismatched reviewed native field navigation proof")
    expected_inputs = {row["path"].split("/")[-1]: row for row in population.geometry_files
                       if row["path"].startswith(("mmaps/", "maps/"))}
    navigation_inputs, terrain_inputs = proof.get("native_navigation_inputs"), proof.get("native_terrain_inputs")
    require(isinstance(navigation_inputs, list) and isinstance(terrain_inputs, list) and
            all(isinstance(row, dict) and isinstance(row.get("path"), str) for row in navigation_inputs + terrain_inputs),
            "Field proof native input lists are missing or invalid")
    supplied_inputs = navigation_inputs + terrain_inputs
    require(len(supplied_inputs) == len(expected_inputs), "Field proof native geometry inputs incomplete")
    seen = set()
    for row in supplied_inputs:
        name = row.get("path", "").split("/")[-1]
        require(name in expected_inputs and name not in seen and
                row.get("sha256") == expected_inputs[name]["sha256"] and row.get("bytes") == expected_inputs[name]["bytes"],
                "Field proof native geometry input changed")
        seen.add(name)
    anchors, routes = navigation_requirements(population)
    results = proof.get("results") or {}
    require(isinstance(results, dict), "Field native navigation results are missing")
    require(results.get("ok") is True and all(type(results.get(key)) is int and results[key] == value
            for key, value in (("map_id", 725), ("poly_ref_bits", 64), ("tiles", 4),
                               ("include_flags", 1), ("exclude_flags", 65534))) and
            results.get("projection_limits") == {"xy": 1, "z": 2},
            "Field proof requires the native 64-bit ground-only query and reviewed local projection limits")
    projections, paths = results.get("projections"), results.get("paths")
    require(isinstance(projections, list) and all(isinstance(row, dict) for row in projections) and len(projections) == len(anchors) and
            results.get("valid_projections") == len(anchors), "Field navigation projection set incomplete")
    projected = set()
    for row in projections:
        identity = row.get("id")
        require(isinstance(identity, str) and identity in anchors and identity not in projected and row.get("valid") is True and
                row.get("input") == anchors[identity] and isinstance(row.get("projected"), dict) and
                all(type(row["projected"].get(key)) in (int, float) and math.isfinite(row["projected"][key])
                    for key in ("x", "y", "z")) and
                math.hypot(row["projected"]["x"] - anchors[identity]["x"],
                           row["projected"]["y"] - anchors[identity]["y"]) <= 1 and
                abs(row["projected"]["z"] - anchors[identity]["z"]) <= 2 and
                isinstance(row.get("status"), dict) and row["status"].get("succeeded") is True and
                row["status"].get("failed") is False and type(row["status"].get("detail_bits")) is int and
                row["status"]["detail_bits"] == 0, "Field navigation anchor changed or failed")
        projected.add(identity)
    require(isinstance(paths, list) and all(isinstance(row, dict) for row in paths) and len(paths) == len(routes) and
            results.get("required_paths") == results.get("complete_paths") == len(routes),
            "Field navigation directed route set incomplete")
    checked = set()
    for row in paths:
        pair = (row.get("from"), row.get("to"))
        require(all(isinstance(identity, str) for identity in pair) and pair in routes and pair not in checked and row.get("complete") is True and
                row.get("all_corridor_flags_ground_only") is True and
                type(row.get("endpoint_delta")) in (int, float) and math.isfinite(row["endpoint_delta"]) and
                0 <= row["endpoint_delta"] <= .01 and all(
                    isinstance(row.get(key), dict) and row[key].get("succeeded") is True and
                    row[key].get("failed") is False and type(row[key].get("detail_bits")) is int and
                    row[key]["detail_bits"] == 0 for key in ("status", "straight_status")),
                "Field navigation path is partial, non-ground, duplicated or changed")
        checked.add(pair)


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(raids.verify_data(data, metadata) == population.raids, "Field geometry differs from reviewed raid baseline")
    verify_navigation(population, metadata)
    return population


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(identity) for identity in sorted(RECRUIT_IDS))
    return raids.snapshot_sql(population.raids) + f"""
SELECT JSON_OBJECT('kind','field_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({ids}) OR spawn_id IN ({ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1, bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)), "Fields require explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
            set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete field SQL read-back")
    recruits = {actor.spawn_id: actor for actor in population.recruits}
    seen = {kind: set() for kind in ("agent", "participation", "field_work")}
    retained = []
    for row in rows:
        kind = row["kind"]
        fields = {"agent": ("id", "spawn", "map", "type", "control", "faction"),
                  "participation": ("spawn",), "field_work": ("id", "spawn")}.get(kind, ())
        require(all(type(row[key]) is int and row[key] >= 0 for key in fields), "Invalid field SQL identity")
        if kind == "agent" and (row["id"] in RECRUIT_IDS or row["spawn"] in RECRUIT_IDS):
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind], "Field agent/spawn collision or duplicate")
            actor = recruits[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) == (725, 0, 725, None, 1) and
                    all(base.near(row[key], expected) for key, expected in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                    "Farmer ordinary type, faction, work or immutable home changed")
            require(row["control"] in (0, 1) and (control is None or row["control"] == control), "Field control phase changed")
            seen[kind].add(identity)
        elif kind == "participation" and row["spawn"] in RECRUIT_IDS:
            identity = row["spawn"]
            require(identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0, "Farmer participation must remain unique FullAgent")
            seen[kind].add(identity)
        elif kind == "field_work":
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind] and
                    all(row[key] is None for key in WORK_FIELDS if key not in ("id", "spawn")), "Farmer workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in RECRUIT_IDS), "Farmer must not acquire group memberships")
            retained.append(row)
    require(seen["field_work"] == seen["agent"], "Field workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == RECRUIT_IDS and seen["participation"] == RECRUIT_IDS, "Field bootstrap incomplete")
    raids.validate_snapshot(retained, population.raids, control=control, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    # The raid adapter already implements narrow, insert-only reservations.
    return raids.bootstrap_sql(population)


def activation_sql(population: Population) -> str:
    bindings = []
    for actor in population.recruits:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type=0 AND a.world_faction_id=1 AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    columns = {"entry": "entry", "ai_name": "AIName", "script_name": "ScriptName",
               "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}
    templates = []
    for template in population.settlement.templates:
        if template["entry"] in {actor.entry for actor in population.recruits}:
            fields = []
            for key in settlement.TEMPLATE_FIELDS:
                value = template[key]
                literal = "'" + value.replace("'", "''") + "'" if isinstance(value, str) else str(value)
                fields.append(f"ct.{columns.get(key, key)}={literal}")
            templates.append("(" + " AND ".join(fields) + ")")
    ready = f"""(SELECT COUNT(*)=12 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.control_mode IN (0,1) AND p.participation_mode=0
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND ({' OR '.join(bindings)})
 AND ({' OR '.join(templates)}))"""
    sql = raids.activation_sql(population.raids).replace(
        f"IN ({population.raids.spawn_ids}", f"IN ({population.spawn_ids}").replace(
        "@raid_population_ready", "@field_population_ready")
    marker = ";\nUPDATE characters.ai_agents"
    require(sql.count(marker) == 1, "Raid activation SQL layout changed")
    return sql.replace(marker, f" AND {ready}" + marker)


def deactivation_sql(population: Population) -> str:
    return raids.deactivation_sql(population)
