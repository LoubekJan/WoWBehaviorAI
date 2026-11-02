"""Reviewed v11 population: 22 household, 3 inn, 10 hall and 10 Defias NPCs.

Every actor uses an independently owned native template. The ten tent actors
clone the project's custom Defias source; no LLM controller or AI agent rows
are enabled. Existing household identities and homes remain immutable.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
from pathlib import Path
import struct

try:
    from . import map_bundle, residents_population as previous, settlement_population as settlement, single_return as base
except ImportError:
    import map_bundle
    import residents_population as previous
    import settlement_population as settlement
    import single_return as base

PROFILE = "residents-v11"
SOURCE_REVISION = "preview-v11"
METADATA = map_bundle.METADATA / SOURCE_REVISION
MANIFEST, POSITION_PROOF = previous.MANIFEST, previous.POSITION_PROOF
FACTION_PROOF = "residents-factions-verification.json"
HOUSE_IDS = previous.HOUSE_IDS
SPAWN_IDS = frozenset(range(902000, 902045))
HOUSEHOLD_IDS = previous.SPAWN_IDS
INN_IDS = frozenset(range(902022, 902025))
HALL_IDS = frozenset(range(902025, 902035))
DEFIAS_IDS = frozenset(range(902035, 902045))
NEW_IDS = SPAWN_IDS - HOUSEHOLD_IDS
ENTRY_BASE = previous.ENTRY_BASE
DEFIAS_BASE_ENTRY = 38
BASE_TEMPLATE = previous.BASE_TEMPLATE
DEFIAS_TEMPLATE = dict(entry=38, ai_name="SmartAI", script_name="", regenerate_health=1,
                       vehicle=0, npcflag=0, unit_flags=0, unit_flags2=2048,
                       faction=2300, minlevel=3, maxlevel=4, flags_extra=0)
TEMPLATES = (DEFIAS_TEMPLATE, BASE_TEMPLATE)
# Filled only from the reviewed source-38 database receipt, never inferred
# from a generic Defias name or copied from another template's behavior.
DEFIAS_SMART_SCRIPTS = ({"kind": "smart_script", **{field: 0 for field in settlement.SMART_FIELDS},
    "entryorguid": 38, "source_type": 0, "event_type": 4, "event_chance": 30,
    "action_type": 1, "action_param3": 1, "target_type": 7,
    "target_x": 0.0, "target_y": 0.0, "target_z": 0.0, "target_o": 0.0,
    "comment": "Defias Thug - On Aggro - Say Line 0 (Phase 1)"},)
require, ProfileError, geometry_digest = base.require, base.ProfileError, previous.geometry_digest


def expected_entry(spawn_id):
    require(type(spawn_id) is int and spawn_id in SPAWN_IDS, "Unknown v11 resident identity")
    return ENTRY_BASE + spawn_id - min(SPAWN_IDS)


def placement(spawn):
    if spawn in HOUSEHOLD_IDS:
        offset = spawn - 902000
        return HOUSE_IDS[offset // 2], offset % 2 + 1, "resident", "stormwind", 11328
    if spawn in INN_IDS: return 209, spawn - 902022 + 1, "inn_resident", "stormwind", 11328
    if spawn in HALL_IDS: return 199, spawn - 902025 + 1, "townhall_resident", "stormwind", 11328
    require(spawn in DEFIAS_IDS, "Unknown residential assignment")
    return 0, spawn - 902035 + 1, "defias_camp", "defias", DEFIAS_BASE_ENTRY


def _point_evidence(point, indoor):
    coordinates = [base.finite(point.get(key)) for key in ("x", "y", "z", "orientation")]
    floor, nav = point.get("geometry"), point.get("navigation")
    require(isinstance(floor, dict) and isinstance(nav, dict) and nav.get("valid") is True and
            floor.get("indoor") is indoor and
            all(floor.get(key) is True for key in ("dry", "full_native_scene_cylinder_clear", "source_all_water_layers_checked")) and
            0 <= coordinates[2] - base.finite(floor.get("floor_z")) <= .15 and
            0 <= base.finite(floor.get("slope_degrees")) <= 15 and
            base.finite(floor.get("clear_radius")) >= .75 and
            0 <= base.finite(floor.get("floor_circle_height_range")) <= .5,
            "V11 NPC standing point lacks dry, clear floor and navigation evidence")
    if indoor:
        require(type(floor.get("group_flags")) is int and floor["group_flags"] & 0x2000 and
                base.finite(floor.get("roof_clearance")) >= 2.3,
                "Indoor v11 NPC point lacks its interior group and clear ceiling")
    else:
        require(base.finite(floor.get("headroom")) >= 2.3,
                "Tent-camp NPC lacks clear outdoor headroom")
    projection = nav.get("projection", {})
    require(isinstance(projection, dict) and projection.get("valid") is True and projection.get("flags") == 1 and
            0 <= base.finite(projection.get("xy_delta")) <= 1 and abs(base.finite(projection.get("z_delta"))) <= 2,
            "V11 resident has no nearby ground navigation projection")
    for key in ("entry_path", "return_path"):
        route = nav.get(key, {})
        require(isinstance(route, dict) and route.get("complete") is True and
                route.get("all_corridor_flags_ground_only") is True and isinstance(route.get("status"), dict) and
                route["status"].get("succeeded") is True and
                all(route["status"].get(flag) is False for flag in ("partial", "buffer_too_small", "out_of_nodes", "failed")),
                "V11 NPC entry/return route is incomplete")
    require(0 <= coordinates[3] < math.tau and
            settlement.ACTIVE_BOUNDS["min_x"] <= coordinates[0] <= settlement.ACTIVE_BOUNDS["max_x"] and
            settlement.ACTIVE_BOUNDS["min_y"] <= coordinates[1] <= settlement.ACTIVE_BOUNDS["max_y"],
            "V11 resident is outside the active scope or has invalid orientation")
    return coordinates


@dataclass(frozen=True)
class Population(previous.Population):
    profile: str = PROFILE

    @staticmethod
    def expected_entry(spawn_id): return expected_entry(spawn_id)

    @classmethod
    def load(cls, metadata: Path = METADATA, geometry=None):
        try:
            geometry = geometry or map_bundle.GeometryBundle.load_revision(metadata.parent, SOURCE_REVISION)
            require(geometry.source_revision == SOURCE_REVISION and geometry.metadata == metadata,
                    "V11 population requires the fixed preview-v11 native geometry")
            document = base.load_json(metadata / MANIFEST)
            require((document.get("profile"), document.get("realm_id"), document.get("map_id"), document.get("area_id"),
                     document.get("source_revision")) == (PROFILE, 2, 725, 4988, SOURCE_REVISION),
                    "Wrong v11 native population identity")
            require(document.get("geometry_sha256") == geometry_digest(geometry.geometry_files) and
                    document.get("templates") == list(TEMPLATES) and document.get("smart_scripts") == list(DEFIAS_SMART_SCRIPTS) and
                    type(document.get("respawn_seconds")) is int and 30 <= document["respawn_seconds"] <= 3600,
                    "V11 native template, script or geometry contract changed")
            factions = metadata / FACTION_PROOF
            require(hashlib.sha256(factions.read_bytes()).hexdigest() == document.get("faction_proof_sha256"),
                    "V11 custom Defias faction proof hash mismatch")
            faction_proof = base.load_json(factions)
            require((faction_proof.get("profile"), faction_proof.get("source_revision"), faction_proof.get("realm_id"),
                     faction_proof.get("map_id"), faction_proof.get("ok")) == (PROFILE, SOURCE_REVISION, 2, 725, True),
                    "Wrong custom Defias faction proof identity")
            proof_path = metadata / POSITION_PROOF
            require(hashlib.sha256(proof_path.read_bytes()).hexdigest() == document.get("position_proof_sha256"),
                    "V11 population position proof hash mismatch")
            proof = base.load_json(proof_path)
            require((proof.get("profile"), proof.get("source_revision"), proof.get("map_id"), proof.get("ok")) ==
                    (PROFILE, SOURCE_REVISION, 725, True) and
                    proof.get("geometry_files") == list(geometry.geometry_files) and
                    proof.get("geometry_sha256") == geometry_digest(geometry.geometry_files),
                    "V11 population proof belongs to different map bytes")
            nav = proof.get("native_navigation", {})
            require(isinstance(nav, dict) and nav.get("ok") is True and
                    all(nav.get(key) == value for key, value in (("map_id", 725), ("poly_ref_bits", 64), ("tiles", 4),
                         ("include_flags", 1), ("exclude_flags", 65534), ("valid_projections", 46),
                         ("required_paths", 90), ("complete_paths", 90))),
                    "V11 requires 45 ground projections and 90 complete bidirectional routes")
            buildings, zones = proof.get("buildings"), proof.get("zones")
            require(isinstance(buildings, list) and len(buildings) == 13 and
                    {row.get("house_id") for row in buildings} == set(HOUSE_IDS) | {199, 209} and
                    isinstance(zones, list) and len(zones) == 1 and zones[0].get("zone_id") == "left-tent-camp",
                    "V11 must contain eleven houses, inn, hall and only the left tent camp")
            files = {row["path"]: row["sha256"] for row in geometry.geometry_files}
            positions = {}
            for building in buildings:
                require(files.get("vmaps/" + str(building.get("mesh_file"))) == building.get("mesh_sha256") and
                        isinstance(building.get("mesh_sha256"), str), "V11 building is outside the native collision closure")
                count = 3 if building["house_id"] == 209 else 10 if building["house_id"] == 199 else 2
                points = building.get("positions")
                require(isinstance(points, list) and len(points) == count and
                        {p.get("slot") for p in points} == set(range(1, count + 1)), "Incorrect v11 indoor population count")
                for point in points:
                    key = (building["house_id"], point["slot"])
                    require(key not in positions, "Duplicate v11 indoor position")
                    positions[key] = point
            camp = zones[0]
            require(camp.get("kind") == "outdoor" and camp.get("source_ids") == [223, 224, 225] and isinstance(camp.get("positions"), list) and
                    len(camp["positions"]) == 10 and {p.get("slot") for p in camp["positions"]} == set(range(1, 11)),
                    "The left tent camp must contain exactly ten NPCs")
            for point in camp["positions"]: positions[(0, point["slot"])] = point
            rows = document.get("actors")
            require(isinstance(rows, list) and len(rows) == len(SPAWN_IDS), "V11 requires exactly 45 native actors")
            actors, seen = [], set()
            old_homes = {actor.spawn_id: actor for actor in previous.Population.load().actors}
            for offset, row in enumerate(rows):
                spawn = 902000 + offset
                house, slot, role, faction, entry = placement(spawn)
                require(isinstance(row, dict) and all(type(row.get(key)) is int for key in ("spawn_id", "entry", "house_id", "slot")) and
                        (row.get("spawn_id"), row.get("entry"), row.get("house_id"), row.get("slot"), row.get("role"), row.get("faction"), row.get("label")) ==
                        (spawn, entry, house, slot, role, faction, f"aiworld_lab_resident_{spawn}"),
                        "V11 actor identity, location assignment or custom Defias faction changed")
                point = positions[(house, slot)]
                require(point.get("spawn_id") == spawn, "V11 proof point belongs to another NPC")
                coordinates = _point_evidence(point, spawn not in DEFIAS_IDS)
                require(all(base.finite(row.get(key)) == value for key, value in zip(("x", "y", "z", "orientation"), coordinates)),
                        "V11 actor differs from its proven standing point")
                if spawn in old_homes:
                    old = old_homes[spawn]
                    require(all(value == getattr(old, key) for key, value in zip(("x", "y", "z", "orientation"), coordinates)),
                            "Existing household homes may not be silently moved by v11")
                require(tuple(coordinates[:3]) not in seen, "V11 NPCs must not share a standing point")
                seen.add(tuple(coordinates[:3]))
                actors.append(previous.Actor(spawn, entry, house, slot, *coordinates, row["label"], faction, role))
            return cls(tuple(actors), geometry, document["respawn_seconds"])
        except (KeyError, TypeError, ValueError, OSError) as exc:
            if isinstance(exc, ProfileError): raise
            raise ProfileError("Missing or invalid v11 native population proof") from exc


def verify_data(data, metadata=METADATA):
    population = Population.load(metadata, map_bundle.verify_data(data, metadata.parent, revision=SOURCE_REVISION))
    verify_factions(data, metadata)
    return population


def verify_factions(data, metadata=METADATA):
    """Verify real server DBC bytes and the approved client's custom identity."""
    document = base.load_json(metadata / MANIFEST)
    proof_path = metadata / FACTION_PROOF
    require(hashlib.sha256(proof_path.read_bytes()).hexdigest() == document.get("faction_proof_sha256"),
            "V11 faction proof changed")
    proof = base.load_json(proof_path)
    require((proof.get("profile"), proof.get("source_revision"), proof.get("ok")) == (PROFILE, SOURCE_REVISION, True),
            "V11 faction proof is not approved")
    files = proof.get("files")
    expected = {"dbc/Faction.dbc":57, "dbc/FactionTemplate.dbc":14}
    require(isinstance(files, list) and len(files) == 2 and {row.get("path") for row in files} == set(expected),
            "V11 faction verification must cover exactly the two faction DBCs")
    loaded = {}
    for row in files:
        raw = (data / row["path"]).read_bytes()
        require(len(raw) >= 20 and row.get("bytes") == len(raw) and
                row.get("sha256") == hashlib.sha256(raw).hexdigest() and row.get("fields") == expected[row["path"]],
                "Installed faction DBC differs from the approved client/server proof")
        magic,count,fields,size,strings = struct.unpack_from("<4s4I",raw)
        require(magic == b"WDBC" and 0 < count < 100000 and fields == expected[row["path"]] and
                size == fields*4 and len(raw) == 20+count*size+strings and strings > 0 and raw[20+count*size] == 0,
                "Invalid Wrath faction DBC layout")
        rows = list(struct.iter_unpack(f"<{fields}I",raw[20:20+count*size]))
        require(len({r[0] for r in rows}) == count,"Faction DBC identities are duplicated")
        loaded[row["path"]] = ({r[0]:r for r in rows},raw[20+count*size:])
    custom = proof.get("custom_defias",{})
    require(isinstance(custom,dict) and all(custom.get(key) == value for key,value in
            (("world_faction_id",2),("faction_id",1201),("faction_template_id",2300),("source_template_id",7),
             ("faction_name","Defias Brotherhood"),("native_reactions_preserved",True))),
            "Custom Defias identity was replaced by a vanilla faction")
    templates = loaded["dbc/FactionTemplate.dbc"][0]
    factions, strings = loaded["dbc/Faction.dbc"]
    require(2300 in templates and 7 in templates and 1201 in factions and
            templates[2300][1] == 1201 and templates[2300][2:] == templates[7][2:] and
            list(templates[2300]) == custom.get("faction_template_row"),
            "Custom Defias template does not preserve its reviewed vanilla reaction profile")
    offset = factions[1201][23]
    end = strings.find(b"\0",offset)
    require(0 <= offset < len(strings) and end > offset and strings[offset:end] == b"Defias Brotherhood",
            "Custom Defias reputation identity is absent from Faction.dbc")
    return proof


def snapshot_sql(population):
    sql = previous.snapshot_sql(population)
    sql = sql.replace("BETWEEN 902000 AND 902021", "BETWEEN 902000 AND 902044")
    sql = sql.replace("WHERE entry=11328;", "WHERE entry IN (38,11328);")
    sql = sql.replace("entryorguid=11328 OR entryorguid BETWEEN -902021 AND -902000",
                      "entryorguid IN (38,11328) OR entryorguid BETWEEN -902044 AND -902000")
    sql = sql.replace("SELECT JSON_OBJECT('kind','smart_script','id',id)",
                      "SELECT JSON_OBJECT('kind','smart_script'," + ",".join(f"'{field}',{field}" for field in settlement.SMART_FIELDS) + ")")
    sql = sql.replace("WHERE entry=11328 AND", "WHERE entry IN (38,11328) AND")
    return sql + "\nSELECT JSON_OBJECT('kind','faction_default','entry',creature_entry,'world_faction',world_faction_id) FROM world.ai_world_faction_entry_defaults WHERE creature_entry BETWEEN 9810000 AND 9810044;\nSELECT JSON_OBJECT('kind','source_faction','entry',creature_entry,'world_faction',world_faction_id) FROM world.ai_world_faction_entry_defaults WHERE creature_entry IN (38,11328);\n"


def validate_snapshot(rows, population, *, bootstrap=False):
    require(isinstance(rows, list) and all(isinstance(row, dict) for row in rows), "Invalid v11 database read-back")
    templates = [row for row in rows if row.get("kind") == "template"]
    require(sorted(templates, key=lambda row: row["entry"]) == [{"kind": "template", **row} for row in TEMPLATES],
            "V11 source native template contracts changed")
    scripts = [row for row in rows if row.get("kind") == "smart_script"]
    require(sorted(scripts, key=settlement.digest) == sorted(DEFIAS_SMART_SCRIPTS, key=settlement.digest),
            "V11 native Defias SmartAI combat contract changed")
    source_factions = [row for row in rows if row.get("kind") == "source_faction"]
    require(sorted(source_factions,key=lambda row:row["entry"]) == [dict(kind="source_faction",entry=38,world_faction=2),
                dict(kind="source_faction",entry=11328,world_faction=1)], "V11 base-entry aliases lost their approved world factions")
    require(all(row.get("kind") in ("template", "spawn", "smart_script", "faction_default", "source_faction") for row in rows),
            "V11 must not acquire retired AI agents, memories, membership or movement overrides")
    spawns, factions = set(), set()
    actors = {actor.spawn_id: actor for actor in population.actors}
    for row in rows:
        if row.get("kind") == "spawn":
            identity = row.get("guid")
            require(type(identity) is int and identity in actors and identity not in spawns, "Foreign or duplicate v11 spawn")
            actor = actors[identity]
            require(all(row.get(key) == value for key, value in (
                ("entry", actor.entry), ("equipment_id", -1), ("map", 725), ("zone", 4988), ("area", 4988),
                ("spawn_mask", 1), ("phase_mask", 1), ("movement", 0), ("wander", 0), ("label", actor.label),
                ("script_name", ""), ("respawn", population.respawn_seconds), ("npcflag", 0), ("unit_flags", 0), ("dynamicflags", 0))) and
                all(base.near(row.get(key), getattr(actor, key)) for key in ("x", "y", "z", "orientation")),
                "V11 immutable home, custom faction source or stationary spawn changed")
            spawns.add(identity)
        elif row.get("kind") == "faction_default":
            entry = row.get("entry")
            require(type(entry) is int and 9810000 <= entry <= 9810044 and entry not in factions and
                    row.get("world_faction") == (2 if entry >= 9810035 else 1), "V11 custom world-faction mapping changed")
            factions.add(entry)
    require(bootstrap or (spawns == SPAWN_IDS and factions == set(range(9810000, 9810045))), "V11 population or custom faction defaults are incomplete")


def bootstrap_sql(population):
    sql = previous.bootstrap_sql(population)
    inserts = []
    for actor in population.actors:
        entry, faction = expected_entry(actor.spawn_id), 2 if actor.spawn_id in DEFIAS_IDS else 1
        inserts.append(f"INSERT INTO world.ai_world_faction_entry_defaults (creature_entry,world_faction_id) SELECT {entry},{faction} WHERE NOT EXISTS(SELECT 1 FROM world.ai_world_faction_entry_defaults WHERE creature_entry={entry});")
    return sql.replace("COMMIT;", "\n".join(inserts) + "\nCOMMIT;")


def validate_telemetry(state, population):
    require(isinstance(state, dict) and state.get("version") == 4 and state.get("stale") is False and
            type(state.get("captured_at_ms")) is int and state["captured_at_ms"] > 0, "V11 native telemetry is stale")
    actors = state.get("agents")
    require(isinstance(actors, list) and len(actors) == 45 and all(isinstance(row, dict) for row in actors) and
            {row.get("spawn_id") for row in actors} == SPAWN_IDS, "V11 telemetry must contain all 45 actors")
    homes = {actor.spawn_id: actor for actor in population.actors}
    for row in actors:
        actor = homes[row["spawn_id"]]
        defias = actor.spawn_id in DEFIAS_IDS
        position, home = row.get("position"), row.get("home")
        require(row.get("native_creature") is True and row.get("control_mode") == "OBSERVE_ONLY" and
                row.get("agent_id") == actor.spawn_id and row.get("world_faction") == (2 if defias else 1) and
                row.get("entry") == actor.entry and row.get("template_entry") == expected_entry(actor.spawn_id) and
                all(row.get(key) is None for key in ("needs", "economy", "living_role", "supply", "raid", "llm_agent")) and
                isinstance(position, dict) and position.get("map_id") == 725 and
                all(type(position.get(key)) in (int, float) and math.isfinite(position[key]) for key in ("x", "y", "z")) and
                isinstance(home, dict) and home.get("map_id") == 725 and
                all(base.near(home.get(key), getattr(actor, key)) for key in ("x", "y", "z")),
                "V11 actor lost its native ownership, immutable home, template or custom faction")
        if position.get("source") == "live":
            require(row.get("faction_template_id") == (2300 if defias else 12), "Live v11 actor has the wrong game faction")
        combat = row.get("in_combat") is True or (isinstance(row.get("movement"), dict) and row["movement"].get("evading") is True)
        require(combat or all(abs(position[key] - getattr(actor, key)) <= .5 for key in ("x", "y", "z")),
                "Idle v11 actor moved away from its reviewed standing point")
    return dict(total=45, managed=0, native=45, live_roles=0, defias=10)
