"""Versioned native-core NPC placement for the authored preview-v8 map.

This profile creates static spawns in the private lab world only. It never
creates AI agents or changes the masked regression population or templates.
A fresh world may omit the historical regression population entirely; an
existing regression population must remain complete and match its proof.
Host deployment must hold its deployment lock and stop the world container;
the TCP guard is an additional check at the private world endpoint.
"""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import socket

try:
    from . import single_return as base
except ImportError:
    import single_return as base

PROFILE = "settlement-v8"
METADATA = base.METADATA / "preview-v8"
MANIFEST = "settlement-population.json"
POSITION_PROOF = "settlement-position-verification.json"
OFFLINE_WORLD_HOST = "aitc-lab-worldserver-1"
SPAWN_IDS = frozenset(range(901000, 901100))
LEGACY_IDS = frozenset(range(900725, 900825))
FACTION_COUNTS = {"stormwind": 44, "defias": 16, "gnolls": 6,
                  "murlocs": 6, "kobolds": 8, "wildlife": 20}
BOUNDS = {"min_x": -533.3333129882812, "max_x": 533.3333129882812,
          "min_y": 0.0, "max_y": 1066.6666259765625}
# Exact outer tile edges would also activate their adjacent missing grids.
ACTIVE_BOUNDS = {key: value + (1 if key.startswith("min_") else -1)
                 for key, value in BOUNDS.items()}
TEMPLATE_FIELDS = "entry ai_name script_name regenerate_health vehicle npcflag unit_flags unit_flags2 faction minlevel maxlevel flags_extra".split()
SMART_FIELDS = ("entryorguid source_type id link event_type event_phase_mask event_chance event_flags "
                "event_param1 event_param2 event_param3 event_param4 event_param5 action_type "
                "action_param1 action_param2 action_param3 action_param4 action_param5 action_param6 "
                "target_type target_param1 target_param2 target_param3 target_param4 "
                "target_x target_y target_z target_o comment").split()
ADDON_FIELDS = "entry path_id mount MountCreatureID StandState AnimTier VisFlags SheathState PvPFlags emote visibilityDistanceType auras".split()
SPAWN_FIELDS = "guid entry equipment_id map zone area spawn_mask phase_mask x y z orientation movement wander label script_name respawn npcflag unit_flags dynamicflags".split()
AGENT_FIELDS = "id spawn map type control home_map home_x home_y home_z home_o work_map faction".split()
FIELDS = {"template": TEMPLATE_FIELDS, "spawn": SPAWN_FIELDS, "agent": AGENT_FIELDS,
          "participation": ["spawn", "mode"], "membership": ["group", "member"], "smart_script": SMART_FIELDS,
          "template_addon": ADDON_FIELDS, "spawn_addon": ["guid"]}
require = base.require
ProfileError = base.ProfileError


def digest(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True).encode("utf-8")).hexdigest()


def integer(value, low: int, high: int, label: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool) and low <= value <= high,
            f"Invalid settlement {label}")
    return value


def sha256(value, label: str) -> str:
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None,
            f"Invalid settlement {label} SHA256")
    return value


@dataclass(frozen=True)
class Actor:
    spawn_id: int
    entry: int
    faction: str
    role: str
    label: str
    x: float
    y: float
    z: float
    orientation: float


@dataclass(frozen=True)
class Population:
    actors: tuple[Actor, ...]
    templates: tuple[dict, ...]
    geometry_files: tuple[dict, ...]
    legacy_snapshot_sha256: str
    respawn_seconds: int
    smart_scripts: tuple[dict, ...] = ()
    template_addons: tuple[dict, ...] = ()
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        document = base.load_json(metadata / MANIFEST)
        require((document.get("profile"), document.get("realm_id"), document.get("map_id"),
                 document.get("area_id"), document.get("source_revision")) ==
                (PROFILE, 2, 725, 4988, "preview-v8"), "Settlement profile/realm/map/source mismatch")
        try:
            require(document["bounds"] == BOUNDS, "Settlement bounds must match authored v8 tiles")
            respawn = integer(document["respawn_seconds"], 30, 3600, "respawn interval")
            legacy_sha = sha256(document["legacy_snapshot_sha256"], "legacy snapshot")
            files = document["geometry_files"]
            server = base.load_json(metadata / "server-manifest.json")
            require(server.get("source_revision") == "preview-v8" and server.get("map_id") == 725 and
                    isinstance(files, list) and len(files) == 61 and files == server["files"],
                    "Settlement geometry must equal the complete 61-file v8 native bundle")
            paths = set()
            for row in files:
                require(isinstance(row, dict), "Invalid settlement geometry record")
                path = row["path"]
                require(isinstance(path, str) and re.fullmatch(r"(?:dbc|maps|vmaps|mmaps)/[A-Za-z0-9_. -]+", path)
                        and path not in paths and not any(part in (".", "..") for part in PurePosixPath(path).parts),
                        "Settlement geometry path collision/escape")
                paths.add(path)
                integer(row["bytes"], 1, 100000000, "geometry bytes")
                sha256(row["sha256"], "geometry")
            templates = document["templates"]
            require(isinstance(templates, list) and templates, "Settlement template contracts missing")
            entries = set()
            for row in templates:
                require(isinstance(row, dict) and set(TEMPLATE_FIELDS).issubset(row),
                        "Incomplete settlement template contract")
                entry = integer(row["entry"], 1, 0xFFFFFF, "template entry")
                require(entry not in entries, "Duplicate settlement template contract")
                entries.add(entry)
                require(row["ai_name"] in ("", "SmartAI") and row["script_name"] == "" and
                        row["regenerate_health"] == 1 and row["vehicle"] == 0,
                        "Settlement requires native core AI, health regeneration and no vehicle")
                for key in ("npcflag", "unit_flags", "unit_flags2", "flags_extra"):
                    integer(row[key], 0, 0xFFFFFFFF, f"template {key}")
                integer(row["faction"], 1, 0xFFFF, "template faction")
                integer(row["minlevel"], 1, 255, "template minlevel")
                integer(row["maxlevel"], row["minlevel"], 255, "template maxlevel")
            smart_scripts = document["smart_scripts"]
            require(isinstance(smart_scripts, list), "Settlement SmartAI contract missing")
            smart_keys = set()
            for row in smart_scripts:
                require(isinstance(row, dict) and row.get("kind") == "smart_script" and
                        set(row) == {"kind", *SMART_FIELDS}, "Incomplete settlement SmartAI contract")
                key = tuple(row[field] for field in ("entryorguid", "source_type", "id", "link"))
                require(key not in smart_keys and row["source_type"] in (0, 9) and
                        row["entryorguid"] > 0 and (row["source_type"] == 9 or row["entryorguid"] in entries),
                        "Settlement SmartAI source/identity collision")
                smart_keys.add(key)
                for field in SMART_FIELDS:
                    if field == "comment":
                        require(isinstance(row[field], str), "Invalid settlement SmartAI comment")
                    elif field in ("target_x", "target_y", "target_z", "target_o"):
                        base.finite(row[field])
                    else:
                        integer(row[field], 0, 0xFFFFFFFF, f"SmartAI {field}")
            action_lists = {row["entryorguid"] for row in smart_scripts if row["source_type"] == 9}
            called_lists = set()
            for row in smart_scripts:
                if row["action_type"] == 80:
                    called_lists.add(row["action_param1"])
                elif row["action_type"] == 87:
                    called_lists.update(row[f"action_param{index}"] for index in range(1, 7)
                                        if row[f"action_param{index}"])
                elif row["action_type"] == 88:
                    low, high = row["action_param1"], row["action_param2"]
                    require(low <= high <= low + 1000, "Unbounded settlement SmartAI action-list range")
                    called_lists.update(range(low, high + 1))
            require(called_lists <= action_lists, "Settlement SmartAI timed action-list closure missing")
            addons = document["template_addons"]
            require(isinstance(addons, list), "Settlement native template-addon contract missing")
            addon_entries = set()
            for row in addons:
                require(isinstance(row, dict) and row.get("kind") == "template_addon" and
                        set(row) == {"kind", *ADDON_FIELDS}, "Incomplete settlement template-addon contract")
                require(row["entry"] in entries and row["entry"] not in addon_entries and
                        row["path_id"] == 0 and row["emote"] == 0,
                        "Settlement template addon identity/path/emote changed")
                addon_entries.add(row["entry"])
                for field in ADDON_FIELDS:
                    if field == "auras":
                        require(row[field] is None or isinstance(row[field], str), "Invalid settlement addon auras")
                    else:
                        integer(row[field], 0, 0xFFFFFFFF, f"template addon {field}")
            raw_actors = document["actors"]
            require(isinstance(raw_actors, list) and len(raw_actors) == 100,
                    "Settlement requires exactly 100 actors")
            actors = []
            for row in raw_actors:
                require(isinstance(row, dict), "Invalid settlement actor")
                spawn = integer(row["spawn_id"], 901000, 901099, "spawn ID")
                entry = integer(row["entry"], 1, 0xFFFFFF, "spawn entry")
                faction, role = row["faction"], row["role"]
                require(entry in entries and isinstance(faction, str) and faction in FACTION_COUNTS and
                        isinstance(role, str) and re.fullmatch(r"[a-z0-9_]{1,48}", role),
                        "Settlement entry/faction/role mismatch")
                label = row["label"]
                require(label == f"aiworld_lab_settlement_v8_{spawn}", "Settlement spawn label mismatch")
                x, y, z, o = (base.finite(row["home"][key]) for key in ("x", "y", "z", "o"))
                require(BOUNDS["min_x"] < x < BOUNDS["max_x"] and
                        BOUNDS["min_y"] < y < BOUNDS["max_y"] and -1000 < z < 1000 and
                        0 <= o < 2 * math.pi, "Settlement home escapes reviewed map")
                actors.append(Actor(spawn, entry, faction, role, label, x, y, z, o))
            require({a.spawn_id for a in actors} == SPAWN_IDS and
                    dict(Counter(a.faction for a in actors)) == FACTION_COUNTS and
                    {a.entry for a in actors} == entries,
                    "Settlement IDs, faction counts or template closure differ")
            require(all(math.hypot(a.x - b.x, a.y - b.y) >= 2.5
                        for i, a in enumerate(actors) for b in actors[i + 1:]),
                    "Settlement homes must be at least 2.5 yards apart")
            proof_path = metadata / POSITION_PROOF
            proof_sha = sha256(document["placement_proof_sha256"], "placement proof")
            require(hashlib.sha256(proof_path.read_bytes()).hexdigest() == proof_sha,
                    "Settlement native placement proof hash mismatch")
            proof = base.load_json(proof_path)
            require((proof.get("status"), proof.get("map_id"), proof.get("source_revision")) ==
                    ("pass", 725, "preview-v8") and proof.get("geometry_files") == files,
                    "Settlement native placement proof map/geometry mismatch")
            points = proof.get("actors")
            require(isinstance(points, list) and len(points) == 100,
                    "Settlement requires all 100 native placement proofs")
            expected = {actor.spawn_id: actor for actor in actors}
            proven = set()
            for point in points:
                require(isinstance(point, dict), "Invalid settlement native placement proof")
                identity = integer(point["spawn_id"], 901000, 901099, "proof spawn ID")
                require(identity in expected and identity not in proven, "Duplicate/unknown native placement proof")
                proven.add(identity)
                actor = expected[identity]
                coordinates = tuple(base.finite(point["home"][field]) for field in ("x", "y", "z", "o"))
                require(point["entry"] == actor.entry and coordinates == (actor.x, actor.y, actor.z, actor.orientation) and
                        all(point.get(field) is True for field in ("dry", "native_ground", "collision_clear")),
                        "Settlement actor coordinates lack matching dry/native/collision placement proof")
        except (KeyError, TypeError) as exc:
            raise ProfileError("Incomplete settlement placement metadata") from exc
        return cls(tuple(sorted(actors, key=lambda a: a.spawn_id)), tuple(templates),
                   tuple(files), legacy_sha, respawn, tuple(smart_scripts), tuple(addons))

    @property
    def spawn_ids(self) -> str:
        return ",".join(str(actor.spawn_id) for actor in self.actors)

    def config(self) -> dict[str, str]:
        require(all(ACTIVE_BOUNDS["min_x"] <= actor.x <= ACTIVE_BOUNDS["max_x"] and
                    ACTIVE_BOUNDS["min_y"] <= actor.y <= ACTIVE_BOUNDS["max_y"] for actor in self.actors),
                "Settlement actors must stay inside the active four-grid scope")
        return {
            "AIWorld.ScopeMapId": "725",
            "AIWorld.ScopeZoneIds": '"4988"',
            "AIWorld.ScopeBoundsEnabled": "1",
            "AIWorld.ScopeMinX": str(ACTIVE_BOUNDS["min_x"]),
            "AIWorld.ScopeMaxX": str(ACTIVE_BOUNDS["max_x"]),
            "AIWorld.ScopeMinY": str(ACTIVE_BOUNDS["min_y"]),
            "AIWorld.ScopeMaxY": str(ACTIVE_BOUNDS["max_y"]),
            "AIWorld.ScopeSpawnIds": f'"{self.spawn_ids}"',
            "AIWorld.ScopeRestrictAgents": "1",
            "AIWorld.ScopeAlwaysActive": "1",
        }

    def observer_environment(self) -> dict[str, str]:
        return {"WORLD_VIEWER_SCOPE_MAP_ID": "725", "WORLD_VIEWER_SCOPE_ZONE_IDS": "4988",
                "WORLD_VIEWER_SCOPE_NAME": "AI World Lab",
                "WORLD_VIEWER_SCOPE_BOUNDS": json.dumps(BOUNDS, separators=(",", ":"))}


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    for row in population.geometry_files:
        path = data / row["path"]
        require(path.is_file() and path.stat().st_size == row["bytes"] and
                hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"],
                f"Settlement live v8 geometry hash mismatch: {row['path']}")
    for directory, patterns in (("maps", ("725*.map",)), ("mmaps", ("725.mmap", "725*.mmtile")),
                                ("vmaps", ("725.vmtree", "725*.vmtile"))):
        actual = {f"{directory}/{p.name}" for pattern in patterns for p in (data / directory).glob(pattern)}
        expected = {row["path"] for row in population.geometry_files
                    if row["path"].startswith(directory + "/") and not row["path"].endswith(".vmo")}
        require(actual == expected, "Unexpected/missing native map-725 geometry footprint")
    return population


def ensure_world_offline(host: str = OFFLINE_WORLD_HOST) -> None:
    require(host == OFFLINE_WORLD_HOST, "Settlement offline guard must target the private lab worldserver")
    # Docker may remove a stopped container from DNS. Deployment captures its
    # private network IP before stopping it, under the same host deployment lock.
    # For a fresh project with no world container, that trusted host-side check
    # can attest absence instead. It only handles Docker's temporary DNS error;
    # the fixed world endpoint is still probed and a listener always vetoes it.
    endpoint = host
    override = os.environ.get("LAB_SETTLEMENT_WORLD_IP")
    absent = os.environ.get("LAB_SETTLEMENT_WORLD_ABSENT")
    require(absent in (None, "0", "1"), "Settlement world absence flag must be 0 or 1")
    require(not (absent == "1" and override is not None),
            "Settlement world absence cannot be combined with a captured world IP")
    if override is not None:
        try:
            address = ipaddress.IPv4Address(override)
        except ipaddress.AddressValueError as exc:
            raise ProfileError("Settlement world IP must be a private IPv4 address") from exc
        require(address.is_private and not address.is_loopback and not address.is_link_local and
                not address.is_unspecified, "Settlement world IP must be a private IPv4 address")
        endpoint = str(address)
    try:
        connection = socket.create_connection((endpoint, 8085), timeout=3)
    except ConnectionRefusedError:
        return
    except socket.gaierror as exc:
        if exc.errno == socket.EAI_NONAME:
            return
        if exc.errno == socket.EAI_AGAIN and absent == "1" and endpoint == host:
            return
        raise ProfileError("Cannot confirm private world listener is stopped") from exc
    except OSError as exc:
        raise ProfileError("Cannot confirm private world listener is stopped") from exc
    else:
        connection.close()
        raise ProfileError("Settlement mutation refused: lab worldserver is accepting connections")


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(i) for i in sorted(SPAWN_IDS | LEGACY_IDS))
    entries = ",".join(str(row["entry"]) for row in population.templates)
    action_lists = ",".join(str(value) for value in sorted({row["entryorguid"] for row in population.smart_scripts
                                                           if row["source_type"] == 9})) or "0"
    overrides = ",".join(str(-value) for value in sorted(SPAWN_IDS))
    smart_json = ",".join(f"'{field}',{field}" for field in SMART_FIELDS)
    addon_json = ",".join(f"'{field}',{field}" for field in ADDON_FIELDS)
    return f"""
SELECT JSON_OBJECT('kind','template','entry',entry,'ai_name',AIName,'script_name',ScriptName,
 'regenerate_health',RegenHealth,'vehicle',VehicleId,'npcflag',npcflag,'unit_flags',unit_flags,
 'unit_flags2',unit_flags2,'faction',faction,'minlevel',minlevel,'maxlevel',maxlevel,'flags_extra',flags_extra)
 FROM world.creature_template WHERE entry IN ({entries});
SELECT JSON_OBJECT('kind','spawn','guid',guid,'entry',id,'equipment_id',equipment_id,'map',map,'zone',zoneId,'area',areaId,
 'spawn_mask',spawnMask,'phase_mask',phaseMask,'x',position_x,'y',position_y,'z',position_z,
 'orientation',orientation,'movement',MovementType,'wander',wander_distance,'label',StringId,
 'script_name',ScriptName,'respawn',spawntimesecs,'npcflag',npcflag,'unit_flags',unit_flags,'dynamicflags',dynamicflags)
 FROM world.creature WHERE map=725 OR guid IN ({ids}) OR StringId LIKE 'aiworld_lab_settlement_%'
 OR StringId='aiworld_lab_single_return' OR StringId LIKE 'aiworld_lab_hunt_%';
SELECT JSON_OBJECT('kind','agent','id',agent_id,'spawn',spawn_id,'map',map_id,'type',agent_type,'control',control_mode,
 'home_map',home_map_id,'home_x',home_x,'home_y',home_y,'home_z',home_z,'home_o',home_o,
 'work_map',work_map_id,'faction',world_faction_id) FROM characters.ai_agents
 WHERE map_id=725 OR agent_id IN ({ids}) OR spawn_id IN ({ids}) OR control_mode=1;
SELECT JSON_OBJECT('kind','participation','spawn',spawn_id,'mode',participation_mode)
 FROM world.ai_spawn_participation_defaults WHERE spawn_id IN ({ids});
SELECT JSON_OBJECT('kind','membership','group',group_id,'member',member_agent_id)
 FROM characters.ai_agent_group_members WHERE member_agent_id IN ({ids});
SELECT JSON_OBJECT('kind','smart_script',{smart_json}) FROM world.smart_scripts
 WHERE (source_type=0 AND entryorguid IN ({entries},{overrides}))
 OR (source_type=9 AND entryorguid IN ({action_lists}));
SELECT JSON_OBJECT('kind','template_addon',{addon_json}) FROM world.creature_template_addon
 WHERE entry IN ({entries});
SELECT JSON_OBJECT('kind','spawn_addon','guid',guid) FROM world.creature_addon
 WHERE guid IN ({population.spawn_ids or ','.join(str(value) for value in sorted(SPAWN_IDS))});
"""


def legacy_snapshot_digest(rows: list[dict]) -> str:
    legacy = [row for row in rows if
              (row.get("kind") == "spawn" and row.get("guid") in LEGACY_IDS) or
              (row.get("kind") == "agent" and (row.get("id") in LEGACY_IDS or row.get("spawn") in LEGACY_IDS)) or
              (row.get("kind") == "participation" and row.get("spawn") in LEGACY_IDS) or
              (row.get("kind") == "membership" and row.get("member") in LEGACY_IDS)]
    return digest(sorted(legacy, key=lambda row: json.dumps(row, sort_keys=True, separators=(",", ":"))))


def validate_snapshot(rows: list[dict], population: Population, *, bootstrap: bool = False) -> None:
    require(all(isinstance(row, dict) and row.get("kind") in FIELDS and
                set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete settlement SQL read-back")
    grouped = {kind: [row for row in rows if row["kind"] == kind] for kind in FIELDS}
    templates = {row["entry"]: row for row in population.templates}
    require(len(grouped["template"]) == len(templates) and
            {row["entry"] for row in grouped["template"]} == set(templates), "Settlement templates missing/duplicated")
    for row in grouped["template"]:
        require(all(row[field] == templates[row["entry"]][field] for field in TEMPLATE_FIELDS),
                "Settlement native template contract changed")
    require(sorted(grouped["smart_script"], key=lambda row: digest(row)) ==
            sorted(population.smart_scripts, key=lambda row: digest(row)),
            "Settlement native SmartAI behavior contract changed")
    require(not grouped["spawn_addon"] and sorted(grouped["template_addon"], key=lambda row: digest(row)) ==
            sorted(population.template_addons, key=lambda row: digest(row)),
            "Settlement native addons changed or reserved spawn has a foreign addon")
    actors = {actor.spawn_id: actor for actor in population.actors}
    seen, legacy_seen = set(), set()
    for row in grouped["spawn"]:
        identity = row["guid"]
        require(identity not in seen | legacy_seen, "Duplicate settlement/legacy spawn")
        if identity in LEGACY_IDS:
            legacy_seen.add(identity)
            label = "aiworld_lab_single_return" if identity == 900725 else f"aiworld_lab_hunt_{identity}"
            require((row["map"], row["zone"], row["area"], row["label"], row["spawn_mask"]) ==
                    (725, 4988, 4988, label, 0), "Legacy hunt population must remain masked and identified")
            continue
        require(identity in actors, "Foreign NPC occupies map 725 or reserved settlement identity")
        seen.add(identity)
        actor = actors[identity]
        require((row["entry"], row["map"], row["zone"], row["area"], row["label"]) ==
                (actor.entry, 725, 4988, 4988, actor.label), "Settlement spawn identity collision")
        require((row["equipment_id"], row["spawn_mask"], row["phase_mask"], row["movement"], row["wander"], row["respawn"],
                 row["npcflag"], row["unit_flags"], row["dynamicflags"], row["script_name"]) ==
                (-1, 1, 1, 0, 0, population.respawn_seconds, 0, 0, 0, "") and
                all(base.near(row[field], value) for field, value in
                    (("x", actor.x), ("y", actor.y), ("z", actor.z), ("orientation", actor.orientation))),
                "Settlement spawn home, mask or native movement contract changed")
    require(bootstrap or seen == SPAWN_IDS, "Settlement bootstrap incomplete")
    require(all(row["spawn"] in LEGACY_IDS and row["mode"] == 0 for row in grouped["participation"]) and
            all(row["member"] in LEGACY_IDS for row in grouped["membership"]),
            "Settlement must not have AI participation or group memberships")
    # New installations do not need to recreate retired test actors. Presence
    # of any legacy record still requires the entire reviewed population, so
    # a partially deleted/migrated population cannot be adopted as a fresh one.
    has_legacy_state = bool(legacy_seen or grouped["participation"] or grouped["membership"] or
                            any(row["id"] in LEGACY_IDS or row["spawn"] in LEGACY_IDS
                                for row in grouped["agent"]))
    if has_legacy_state:
        require(legacy_seen == LEGACY_IDS, "Masked 100-actor legacy population is incomplete")
        require(len(grouped["agent"]) == 100 and
                {row["id"] for row in grouped["agent"]} == LEGACY_IDS and
                all(row["spawn"] == row["id"] and row["map"] == 725 and row["control"] == 0
                    for row in grouped["agent"]), "Only the 100 disabled legacy AI agents may exist")
        require(legacy_snapshot_digest(rows) == population.legacy_snapshot_sha256,
                "Legacy regression identities/homes/control changed")
    else:
        require(not grouped["agent"], "Native settlement must not have AI agents")


def bootstrap_sql(population: Population) -> str:
    statements = ["START TRANSACTION;"]
    for actor in population.actors:
        statements.append(f"""INSERT INTO world.creature (guid,id,equipment_id,map,zoneId,areaId,spawnMask,phaseMask,
 position_x,position_y,position_z,orientation,spawntimesecs,wander_distance,MovementType,curhealth,curmana,StringId,VerifiedBuild)
 SELECT {actor.spawn_id},{actor.entry},-1,725,4988,4988,1,1,
 {actor.x},{actor.y},{actor.z},{actor.orientation},{population.respawn_seconds},0,0,0,0,'{actor.label}',12340
 WHERE NOT EXISTS(SELECT 1 FROM world.creature WHERE guid={actor.spawn_id});""")
    statements.append("COMMIT;")
    return "\n".join(statements) + "\n"
