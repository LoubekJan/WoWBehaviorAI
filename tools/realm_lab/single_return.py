"""Explicit one-bear lab activation. No shared-auth SQL or global migrations.

Geometry and home come from the reviewed map metadata. Database writes are
generated here, run only by manage.py against lab-mysql, and retain legacy rows.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
METADATA = ROOT / "data/realm_lab/aiworldlab"
SPAWN_ID = 900725
ENTRY = 1186
SPAWN_LABEL = "aiworld_lab_single_return"
LEGACY_PILOTS = frozenset((80335, 214023, 214021, 80683))


class ProfileError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ProfileError(message)


def load_json(path: Path) -> dict:
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ProfileError(f"Missing/invalid reviewed metadata: {path.name}") from exc
    require(isinstance(result, dict) and result.get("schema_version") == 1,
            f"Unsupported metadata: {path.name}")
    return result


def finite(value) -> float:
    require(isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value),
            "Scope coordinates must be finite numbers")
    return float(value)


@dataclass(frozen=True)
class Scope:
    map_id: int
    area_id: int
    home_x: float
    home_y: float
    home_z: float
    min_x: float
    max_x: float
    min_y: float
    max_y: float
    grid_x: int
    grid_y: int

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Scope:
        points = load_json(metadata / "test-points.json")
        require((points.get("map_id"), points.get("area_id")) == (725, 4988),
                "Only the reviewed map 725 / area 4988 may use single-return")
        try:
            region, bounds, home, tile = (points[key] for key in ("test_region", "bounds", "home", "tile"))
            x, y, z = (finite(home[key]) for key in ("x", "y", "floor_z"))
            lo_x, hi_x, lo_y, hi_y = (finite(region[key]) for key in ("x_min", "x_max", "y_min", "y_max"))
            require(lo_x < hi_x and lo_y < hi_y and z == 0 and
                    math.isclose(x, (lo_x + hi_x) / 2, abs_tol=0.001) and
                    math.isclose(y, (lo_y + hi_y) / 2, abs_tol=0.001), "Invalid fixed home / lab region")
            require(finite(bounds["x_min"]) <= lo_x < hi_x <= finite(bounds["x_max"]) and
                    finite(bounds["y_min"]) <= lo_y < hi_y <= finite(bounds["y_max"]),
                    "Lab region escapes the reviewed terrain tile")
            require((tile["adt_x"], tile["adt_y"], tile["server_grid_x"], tile["server_grid_y"]) == (30, 31, 31, 30),
                    "Unexpected lab tile coordinates")
            return_points = points["return_points"]
            require(len(return_points) == 4, "Four reviewed return points required")
            for point in return_points:
                px, py = finite(point["x"]), finite(point["y"])
                require(lo_x <= px <= hi_x and lo_y <= py <= hi_y and finite(point["floor_z"]) == z and
                        math.isclose(math.hypot(px - x, py - y), 40, abs_tol=0.001),
                        "Invalid reviewed return point")
        except (KeyError, TypeError) as exc:
            raise ProfileError("Incomplete reviewed scope metadata") from exc
        return cls(725, 4988, x, y, z, lo_x, hi_x, lo_y, hi_y, 31, 30)

    def config(self) -> dict[str, str]:
        return {
            "AIWorld.ScopeMapId": str(self.map_id),
            "AIWorld.ScopeZoneIds": f'"{self.area_id}"',
            "AIWorld.ScopeBoundsEnabled": "1",
            "AIWorld.ScopeMinX": str(self.min_x), "AIWorld.ScopeMaxX": str(self.max_x),
            "AIWorld.ScopeMinY": str(self.min_y), "AIWorld.ScopeMaxY": str(self.max_y),
            "AIWorld.ScopeSpawnIds": f'"{SPAWN_ID}"',
            "AIWorld.ScopeRestrictAgents": "1", "AIWorld.ScopeAlwaysActive": "1",
        }

    def observer_environment(self) -> dict[str, str]:
        return {
            "WORLD_VIEWER_SCOPE_MAP_ID": str(self.map_id),
            "WORLD_VIEWER_SCOPE_ZONE_IDS": str(self.area_id),
            "WORLD_VIEWER_SCOPE_NAME": "AI World Lab",
            "WORLD_VIEWER_SCOPE_BOUNDS": json.dumps({
                "min_x": self.min_x, "max_x": self.max_x, "min_y": self.min_y, "max_y": self.max_y,
            }, separators=(",", ":")),
        }


def read_dbc(data: bytes, fields: int):
    require(len(data) >= 20, "Truncated live DBC")
    magic, count, actual, size, string_size = struct.unpack_from("<4s4I", data)
    require(magic == b"WDBC" and actual == fields and size == fields * 4 and
            len(data) == 20 + count * size + string_size, "Invalid live DBC layout")
    rows = [struct.unpack_from(f"<{fields}I", data, 20 + index * size) for index in range(count)]
    require(len({row[0] for row in rows}) == count, "Duplicate live DBC IDs")
    strings = data[20 + count * size:]
    require(strings.startswith(b"\0") and strings.endswith(b"\0"), "Invalid live DBC strings")
    return rows, strings


def verify_data(data: Path, metadata: Path = METADATA) -> Scope:
    try:
        return _verify_data(data, metadata)
    except (KeyError, TypeError, AttributeError) as exc:
        raise ProfileError("Incomplete/invalid reviewed bundle proof") from exc


def _verify_data(data: Path, metadata: Path) -> Scope:
    """Verify the six live hashes and separate geometry/native acceptance."""
    scope = Scope.load(metadata)
    project = load_json(metadata / "project-manifest.json")
    server = load_json(metadata / "server-manifest.json")
    native = load_json(metadata / "in-game-validation.json")
    points = load_json(metadata / "test-points.json")
    for document in (project, server, native, points):
        require((document.get("map_id"), document.get("area_id")) == (scope.map_id, scope.area_id),
                "Map/area proof mismatch")
    require(len({document.get("source_revision") for document in (project, server, native, points)}) == 1 and
            project.get("source_revision"), "Map source revision proof mismatch")
    require(native.get("realm_id") == 2 and
            native.get("player_confirmation", {}).get("map_entry_and_walking") == "confirmed_by_user" and
            native.get("npc_test", {}).get("status") == "confirmed_by_user" and
            native["npc_test"].get("entry") == ENTRY and
            native["npc_test"].get("template_verified_in_lab_database") is True,
            "Native player entry and NPC follow/evade confirmation required")
    nav = server.get("navigation", {})
    paths = nav.get("paths", [])
    require(nav.get("ok") is True and nav.get("scope") == "navmesh_only" and nav.get("map_id") == scope.map_id and
            (nav.get("projected_points"), nav.get("complete_paths"), nav.get("required_paths")) == (5, 8, 8) and
            len(paths) == 8 and all(path.get("complete") is True for path in paths),
            "Eight complete reviewed navmesh paths required")
    require(server.get("client_patch_sha256") == project.get("client", {}).get("patch", {}).get("sha256") and
            server.get("deployment", {}).get("status") == "installed_and_healthy",
            "Installed server/client bundle proof mismatch")
    tile = f"{scope.map_id:03}{scope.grid_x:02}{scope.grid_y:02}"
    expected = {"dbc/Map.dbc", "dbc/AreaTable.dbc", "dbc/Light.dbc", f"maps/{tile}.map",
                f"mmaps/{scope.map_id:03}.mmap", f"mmaps/{tile}.mmtile"}
    files = server.get("files", [])
    require(len(files) == 6 and {item.get("path") for item in files} == expected,
            "The six reviewed server files are required")
    root = data.resolve()
    contents = {}
    for item in files:
        path = (root / item["path"]).resolve()
        require(path.is_relative_to(root), "Live map file escapes lab DataDir")
        try:
            content = path.read_bytes()
        except OSError as exc:
            raise ProfileError(f"Missing live lab file: {item['path']}") from exc
        require(len(content) == item.get("bytes") and hashlib.sha256(content).hexdigest() == item.get("sha256"),
                f"Live lab file differs from reviewed bundle: {item['path']}")
        contents[item["path"]] = content
    require({path.name for path in (root / "maps").glob(f"{scope.map_id:03}*.map")} == {f"{tile}.map"} and
            {path.name for path in (root / "mmaps").glob(f"{scope.map_id:03}*")} ==
            {f"{scope.map_id:03}.mmap", f"{tile}.mmtile"}, "Unexpected additional lab terrain/navmesh tile")
    require(server.get("custom_vmaps", {}).get("status") == "expected_absent" and
            server["custom_vmaps"].get("objects") == 0 and not list((root / "vmaps").glob(f"{scope.map_id:03}*")),
            "This first lab profile requires reviewed terrain-only geometry")
    maps, strings = read_dbc(contents["dbc/Map.dbc"], 66)
    selected = [row for row in maps if row[0] == scope.map_id]
    require(len(selected) == 1, "Missing live map 725")
    row = selected[0]
    require(row[1] < len(strings) and strings[row[1]:].split(b"\0", 1)[0] == b"AIWorldLab" and
            (row[2], row[22], row[63]) == (0, scope.area_id, 2), "Wrong live Map.dbc binding")
    areas, _ = read_dbc(contents["dbc/AreaTable.dbc"], 36)
    selected = [row for row in areas if row[0] == scope.area_id]
    require(len(selected) == 1 and selected[0][1:3] == (scope.map_id, 0) and selected[0][3] < 4096 and
            sum(row[3] == selected[0][3] for row in areas) == 1, "Wrong live AreaTable binding/exploration bit")
    return scope


def snapshot_sql(scope: Scope) -> str:
    """Read only lab world/characters; JSON rows avoid locale/tab ambiguity."""
    return f"""
SELECT JSON_OBJECT('kind','template','entry',entry,'ai_name',AIName,'script_name',ScriptName,
 'regenerate_health',RegenHealth,'vehicle',VehicleId,'npcflag',npcflag) FROM world.creature_template WHERE entry={ENTRY};
SELECT JSON_OBJECT('kind','spawn','guid',guid,'entry',id,'map',map,'zone',zoneId,'area',areaId,
 'spawn_mask',spawnMask,'phase_mask',phaseMask,'x',position_x,'y',position_y,'z',position_z,
 'orientation',orientation,'movement',MovementType,'wander',wander_distance,'label',StringId,'script_name',ScriptName)
 FROM world.creature WHERE guid={SPAWN_ID} OR map={scope.map_id} OR StringId='{SPAWN_LABEL}';
SELECT JSON_OBJECT('kind','agent','id',agent_id,'spawn',spawn_id,'map',map_id,'type',agent_type,'control',control_mode,
 'home_map',home_map_id,'home_x',home_x,'home_y',home_y,'home_z',home_z,'home_o',home_o,
 'work_map',work_map_id,'faction',world_faction_id) FROM characters.ai_agents
 WHERE agent_id={SPAWN_ID} OR spawn_id={SPAWN_ID} OR map_id={scope.map_id} OR control_mode=1;
SELECT JSON_OBJECT('kind','participation','spawn',spawn_id,'mode',participation_mode)
 FROM world.ai_spawn_participation_defaults WHERE spawn_id={SPAWN_ID};
SELECT JSON_OBJECT('kind','type','entry',creature_entry,'type',agent_type)
 FROM world.ai_agent_type_entry_defaults WHERE creature_entry={ENTRY};
SELECT JSON_OBJECT('kind','membership','group',group_id,'member',member_agent_id)
 FROM characters.ai_agent_group_members WHERE member_agent_id={SPAWN_ID};
"""


def parse_snapshot(output: str) -> list[dict]:
    try:
        rows = [json.loads(line) for line in output.splitlines() if line.strip()]
    except ValueError as exc:
        raise ProfileError("Invalid lab SQL read-back") from exc
    require(all(isinstance(row, dict) and "kind" in row for row in rows), "Invalid lab SQL read-back")
    return rows


def near(value, expected: float) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and abs(value - expected) <= 0.001


def validate_snapshot(rows: list[dict], scope: Scope, *, control: int | None,
                      bootstrap: bool = False) -> None:
    fields = {
        "template": "entry ai_name script_name regenerate_health vehicle npcflag",
        "spawn": "guid entry map zone area spawn_mask phase_mask x y z orientation movement wander label script_name",
        "agent": "id spawn map type control home_map home_x home_y home_z home_o work_map faction",
        "participation": "spawn mode", "type": "entry type", "membership": "group member",
    }
    require(all(isinstance(row, dict) and row.get("kind") in fields and
                set(fields[row["kind"]].split()).issubset(row) for row in rows),
            "Incomplete lab SQL read-back")
    groups = {kind: [row for row in rows if row["kind"] == kind]
              for kind in ("template", "spawn", "agent", "participation", "type", "membership")}
    require(len(groups["template"]) == 1, "Lab bear template 1186 missing")
    template = groups["template"][0]
    require(template["entry"] == ENTRY and not template["ai_name"] and not template["script_name"] and template["regenerate_health"] == 1 and
            template["vehicle"] == 0 and template["npcflag"] == 0, "Bear template must be native, healthy wildlife")
    require(not groups["membership"], "Lab bear must not have an AI group membership")
    require(len(groups["spawn"]) <= 1, "Only one persistent spawn is permitted on the lab map")
    for row in groups["spawn"]:
        require((row["guid"], row["entry"], row["map"], row["zone"], row["area"], row["label"]) ==
                (SPAWN_ID, ENTRY, scope.map_id, scope.area_id, scope.area_id, SPAWN_LABEL),
                "Lab spawn ID/label/map collision")
        require((row["spawn_mask"], row["phase_mask"], row["movement"], row["wander"]) == (1, 1, 0, 0) and
                not row["script_name"] and near(row["orientation"], 0) and
                all(near(row[key], expected) for key, expected in
                    (("x", scope.home_x), ("y", scope.home_y), ("z", scope.home_z))),
                "Lab spawn home or movement changed")
    selected_agents = []
    for row in groups["agent"]:
        if row["id"] != SPAWN_ID and row["spawn"] != SPAWN_ID and row["map"] != scope.map_id:
            require(bootstrap and row["control"] == 1 and row["map"] == 0 and row["id"] == row["spawn"] and
                    row["id"] in LEGACY_PILOTS, "An outside agent is AIWorldControlled")
            continue
        selected_agents.append(row)
        require((row["id"], row["spawn"], row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                (SPAWN_ID, SPAWN_ID, scope.map_id, 6, scope.map_id, None, 0), "Lab agent identity/role collision")
        require(all(near(row[key], expected) for key, expected in
                    (("home_x", scope.home_x), ("home_y", scope.home_y), ("home_z", scope.home_z), ("home_o", 0))),
                "Lab agent home changed")
        require(row["control"] in (0, 1) and (control is None or row["control"] == control),
                "Lab control mode differs from activation phase")
    require(len(selected_agents) <= 1, "Multiple lab agents are forbidden")
    require(len(groups["participation"]) <= 1 and
            all((row["spawn"], row["mode"]) == (SPAWN_ID, 0) for row in groups["participation"]),
            "Lab participation differs from FullAgent")
    require(len(groups["type"]) <= 1 and
            all((row["entry"], row["type"]) == (ENTRY, 6) for row in groups["type"]),
            "Lab entry classification differs from Predator")
    if not bootstrap:
        require(len(groups["spawn"]) == len(selected_agents) == len(groups["participation"]) == len(groups["type"]) == 1,
                "Single-return bootstrap has not been completed")


def bootstrap_sql(scope: Scope) -> str:
    legacy = ",".join(str(value) for value in sorted(LEGACY_PILOTS))
    return f"""START TRANSACTION;
INSERT INTO world.creature (guid,id,map,zoneId,areaId,spawnMask,phaseMask,position_x,position_y,position_z,
 orientation,wander_distance,MovementType,StringId,VerifiedBuild)
 SELECT {SPAWN_ID},{ENTRY},{scope.map_id},{scope.area_id},{scope.area_id},1,1,{scope.home_x},{scope.home_y},{scope.home_z},
 0,0,0,'{SPAWN_LABEL}',12340 WHERE NOT EXISTS(SELECT 1 FROM world.creature WHERE guid={SPAWN_ID});
INSERT INTO world.ai_spawn_participation_defaults (spawn_id,participation_mode)
 SELECT {SPAWN_ID},0 WHERE NOT EXISTS(SELECT 1 FROM world.ai_spawn_participation_defaults WHERE spawn_id={SPAWN_ID});
INSERT INTO world.ai_agent_type_entry_defaults (creature_entry,agent_type)
 SELECT {ENTRY},6 WHERE NOT EXISTS(SELECT 1 FROM world.ai_agent_type_entry_defaults WHERE creature_entry={ENTRY});
INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,home_map_id,home_x,home_y,home_z,home_o,world_faction_id)
 SELECT {SPAWN_ID},6,{scope.map_id},{SPAWN_ID},0,{scope.map_id},{scope.home_x},{scope.home_y},{scope.home_z},0,0
 WHERE NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE agent_id={SPAWN_ID} OR (map_id={scope.map_id} AND spawn_id={SPAWN_ID}));
UPDATE characters.ai_agents SET control_mode=0 WHERE map_id=0 AND agent_id=spawn_id AND spawn_id IN ({legacy}) AND control_mode=1;
UPDATE characters.ai_agents SET control_mode=0 WHERE agent_id={SPAWN_ID} AND spawn_id={SPAWN_ID} AND map_id={scope.map_id};
COMMIT;
"""


def activation_sql(scope: Scope) -> str:
    return f"""START TRANSACTION;
UPDATE characters.ai_agents a INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 INNER JOIN world.ai_agent_type_entry_defaults t ON t.creature_entry=c.id
 LEFT JOIN characters.ai_agent_group_members g ON g.member_agent_id=a.agent_id
 LEFT JOIN characters.ai_agents other ON other.control_mode=1 AND other.agent_id<>{SPAWN_ID}
 LEFT JOIN world.creature extra ON extra.map={scope.map_id} AND extra.guid<>{SPAWN_ID}
 SET a.control_mode=1 WHERE a.agent_id={SPAWN_ID} AND a.spawn_id={SPAWN_ID} AND a.map_id={scope.map_id}
 AND a.agent_type=6 AND a.control_mode=0 AND c.id={ENTRY} AND c.StringId='{SPAWN_LABEL}'
 AND c.zoneId={scope.area_id} AND c.areaId={scope.area_id} AND p.participation_mode=0 AND t.agent_type=6
 AND g.member_agent_id IS NULL AND other.agent_id IS NULL AND extra.guid IS NULL
 AND c.spawnMask=1 AND c.phaseMask=1 AND c.MovementType=0 AND c.wander_distance=0 AND c.ScriptName=''
 AND ct.AIName='' AND ct.ScriptName='' AND ct.RegenHealth=1 AND ct.VehicleId=0 AND ct.npcflag=0
 AND ABS(c.position_x-{scope.home_x})<=0.001 AND ABS(c.position_y-{scope.home_y})<=0.001
 AND ABS(c.position_z-{scope.home_z})<=0.001 AND ABS(c.orientation)<=0.001
 AND a.home_map_id={scope.map_id} AND a.work_map_id IS NULL AND a.world_faction_id=0
 AND ABS(a.home_x-{scope.home_x})<=0.001 AND ABS(a.home_y-{scope.home_y})<=0.001
 AND ABS(a.home_z-{scope.home_z})<=0.001 AND ABS(a.home_o)<=0.001;
COMMIT;
"""
