"""Explicit AIWorld control for the 45 independently authored v11 residents.

The reviewed native population, its homes, and all owned character templates
remain unchanged.  Only participation and agent identities are installed;
retired v8 actors are never bootstrapped.  LLM cards remain an independent opt-in.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
from pathlib import Path

try:
    from . import residents_v11 as native, settlement_population as settlement, single_return as base
except ImportError:
    import residents_v11 as native
    import settlement_population as settlement
    import single_return as base

PROFILE = "living-v11"
METADATA = native.METADATA
MANIFEST = "living-population.json"
POSITION_PROOF = "living-position-verification.json"
LOCATIONS_HEADER = Path(__file__).resolve().parents[2] / "src/server/game/AIWorld/Agent/LivingResidentsV11Locations.h"
SPAWN_IDS = native.SPAWN_IDS
DEFIAS_IDS = native.DEFIAS_IDS
HUMAN_IDS = SPAWN_IDS - DEFIAS_IDS
require, ProfileError = base.require, base.ProfileError
WORK_FIELDS = ("work_map", "work_x", "work_y", "work_z", "work_o")
AGENT_FIELDS = ("id", "spawn", "map", "type", "control", "home_map", "home_x", "home_y", "home_z", "home_o", "faction", *WORK_FIELDS)


def agent_type(spawn):
    require(type(spawn) is int and spawn in SPAWN_IDS, "Unknown living v11 actor")
    return 5 if spawn in DEFIAS_IDS else 0


def world_faction(spawn):
    return 2 if spawn in DEFIAS_IDS else 1


def capability(spawn):
    return "farmer" if spawn < 902012 else "village_consumer" if spawn < 902016 else "city_carrier" if spawn < 902018 else "city_consumer" if spawn < 902035 else "raider"


def depot_id(spawn):
    return 1 + (spawn-902000)//4 if spawn < 902016 else 5 if spawn < 902035 else 6


def runtime_catalog(proof):
    """Only the geometry-backed values compiled into the runtime catalog."""
    point = lambda value: dict(map_id=725,**{key:value[key] for key in ("x","y","z")})
    return dict(depots=[dict(id=d["id"],name=d["name"],source_unique_id=d["source_unique_id"],
                    location_kind=d["location_kind"],position=d["position"]) for d in proof["depots"]],
        fields=[dict(id=f["id"],work_points=[point(p) for p in f["work_points"]]) for f in proof["fields"]],
        worker_assignments=proof["worker_assignments"],
        routes=[dict(from_depot_id=r["from_depot_id"],to_depot_id=r["to_depot_id"],
                     points=[point(p) for p in r["points"]]) for r in proof["runtime_routes"]])


def position_contract(population, header=None):
    proof_path=population.metadata / POSITION_PROOF
    raw=proof_path.read_bytes()
    require(0<len(raw)<=16*1024*1024, "Living position proof is absent or exceeds its bound")
    proof=base.load_json(proof_path)
    header=header or LOCATIONS_HEADER
    raw_header=header.read_bytes()
    sha=hashlib.sha256(raw).hexdigest()
    return proof,dict(position_proof_sha256=sha,locations_header_sha256=hashlib.sha256(raw_header).hexdigest(),
                      runtime_catalog_sha256=settlement.digest(runtime_catalog(proof)))


def verify_positions(proof,population,contract,header=None):
    require((proof.get("schema_version"),proof.get("profile"),proof.get("source_revision"),proof.get("realm_id"),
        proof.get("map_id"),proof.get("area_id"),proof.get("ok"))==(1,PROFILE,native.SOURCE_REVISION,2,725,4988,True),
        "Living position proof identity is invalid")
    require(proof.get("geometry_sha256")==native.geometry_digest(population.geometry_files) and
        proof.get("geometry_files")==list(population.geometry_files) and
        proof.get("resident_position_proof_sha256")==hashlib.sha256((population.metadata/native.POSITION_PROOF).read_bytes()).hexdigest() and
        proof.get("homes_unchanged") is True and proof.get("source_map_modified") is False and
        proof.get("source_all_water_layers_checked") is True and proof.get("runtime_route_legs_verified") is True,
        "Living interaction proof belongs to another geometry or changes authored homes")
    require(f'ProofSha256[] = "{contract["position_proof_sha256"]}"' in (header or LOCATIONS_HEADER).read_text(encoding="utf-8"),
            "Compiled living locations are not bound to the same reviewed position proof")
    for key,projections,paths in (("native_navigation",87,346),("waypoint_navigation",70,144)):
        nav=proof.get(key,{})
        require(isinstance(nav,dict) and all(nav.get(k)==v for k,v in
            (("ok",True),("map_id",725),("poly_ref_bits",64),("tiles",4),("include_flags",1),("exclude_flags",65534),
             ("valid_projections",projections),("required_paths",paths),("complete_paths",paths))) and
            isinstance(nav.get("paths"),list) and len(nav["paths"])==paths and all(
                p.get("complete") is True and p.get("all_corridor_flags_ground_only") is True and
                p.get("status",{}).get("succeeded") is True and all(p["status"].get(k) is False for k in
                    ("partial","buffer_too_small","out_of_nodes","failed")) for p in nav["paths"]),
            "Living interaction graph contains incomplete or non-ground native routes")
    depots,fields=proof.get("depots"),proof.get("fields")
    require(isinstance(depots,list) and [d.get("id") for d in depots]==list(range(1,7)) and
            [d.get("source_unique_id") for d in depots]==[180,186,212,None,None,None] and
            isinstance(fields,list) and [f.get("id") for f in fields]==[1,2,3] and
            all(isinstance(f.get("work_points"),list) and len(f["work_points"])==12 for f in fields),
            "Living layout must contain the three real granaries/fields and six reviewed depots")
    checked=[(d["position"],d.get("geometry",{})) for d in depots]
    for field in fields:
        for point in field["work_points"]:
            require(type(point.get("crop_alpha")) is int and point["crop_alpha"]>=128,
                    "Living field work point is not on an authored crop")
            checked.append((point,point.get("geometry",{})))
    for point,geo in checked:
        require(all(type(point.get(k)) in (int,float) and math.isfinite(point[k]) for k in ("x","y","z")) and
            settlement.ACTIVE_BOUNDS["min_x"]<=point["x"]<=settlement.ACTIVE_BOUNDS["max_x"] and
            settlement.ACTIVE_BOUNDS["min_y"]<=point["y"]<=settlement.ACTIVE_BOUNDS["max_y"] and
            all(geo.get(k) is True for k in ("full_native_scene_cylinder_clear","source_all_water_layers_checked","dry")) and
            base.finite(geo.get("clear_radius"))>=.75 and base.finite(geo.get("headroom"))>=2.3 and
            0<=point["z"]-base.finite(geo.get("floor_z"))<=.15,
            "Living field or depot lacks a dry, clear, supported interaction point")
    expected_workers=[dict(spawn_id=i,field_id=depot_id(i),depot_id=depot_id(i),
        work_point_indices=[(i-902000)%4+4*c for c in range(3)]) for i in range(902000,902012)]
    require(proof.get("worker_assignments")==expected_workers,"Living field worker assignment changed")
    expected_routes={(a,b) for a,b in ((1,5),(2,5),(3,5),(4,5),(6,1),(6,2),(6,3),(6,4)) for a,b in ((a,b),(b,a))}
    routes=proof.get("runtime_routes")
    require(isinstance(routes,list) and len(routes)==16 and
            {(r.get("from_depot_id"),r.get("to_depot_id")) for r in routes}==expected_routes,
            "Living carriers or raiders lack the exact reviewed directed route catalog")
    by_depot={d['id']:d['position'] for d in depots}
    for route in routes:
        points=route.get("points")
        require(isinstance(points,list) and 2<=len(points)<=64 and all(
            type(p.get(k)) in (int,float) and math.isfinite(p[k]) for p in points for k in ("x","y","z")) and
            all(base.near(points[0][k],by_depot[route["from_depot_id"]][k]) and
                base.near(points[-1][k],by_depot[route["to_depot_id"]][k]) for k in ("x","y","z")) and
            all(math.dist((a['x'],a['y'],a['z']),(b['x'],b['y'],b['z']))<=160 for a,b in zip(points,points[1:])),
            "Living runtime route endpoints or bounded physical legs changed")


def manifest_document(population, native_manifest_sha256, contract=None):
    if contract is None: _proof,contract=position_contract(population)
    return dict(schema_version=1, profile=PROFILE, realm_id=2, map_id=725, area_id=4988,
        source_revision=native.SOURCE_REVISION, native_profile=native.PROFILE,
        native_manifest_sha256=native_manifest_sha256,
        geometry_sha256=native.geometry_digest(population.geometry_files),
        owned_spawn_ids=sorted(SPAWN_IDS), native_spawn_ids=[], immutable_spawn_homes=True,
        **contract,
        llm_cards_explicit_opt_in=True, runtime_food_state_only=True,
        actors=[dict(spawn_id=a.spawn_id, entry=native.expected_entry(a.spawn_id), base_entry=a.entry,
            agent_type=agent_type(a.spawn_id), world_faction_id=world_faction(a.spawn_id),
            capability=capability(a.spawn_id),depot_id=depot_id(a.spawn_id),
            home=dict(x=a.x,y=a.y,z=a.z,o=a.orientation)) for a in population.actors])


@dataclass(frozen=True)
class Population:
    native_population: native.Population
    positions: dict | None = None
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA, geometry=None):
        population = native.Population.load(metadata, geometry)
        proof,contract=position_contract(population)
        verify_positions(proof,population,contract)
        sha = hashlib.sha256((metadata / native.MANIFEST).read_bytes()).hexdigest()
        require(settlement.digest(base.load_json(metadata / MANIFEST)) ==
                settlement.digest(manifest_document(population, sha,contract)),
                "Living v11 identity, source population, geometry or immutable homes changed")
        return cls(population,proof)

    @property
    def actors(self): return self.native_population.actors
    @property
    def settlement(self): return self.native_population
    @property
    def spawn_ids(self): return self.native_population.spawn_ids
    @property
    def geometry(self): return self.native_population.geometry
    @property
    def geometry_files(self): return self.native_population.geometry_files
    @property
    def source_revision(self): return self.native_population.source_revision
    @property
    def metadata(self): return self.native_population.metadata
    @property
    def entry_proof(self): return self.native_population.entry_proof
    @property
    def entry_proof_sha256(self): return self.native_population.entry_proof_sha256
    @property
    def respawn_seconds(self): return self.native_population.respawn_seconds
    @staticmethod
    def expected_entry(spawn_id): return native.expected_entry(spawn_id)

    def config(self):
        return {**self.native_population.config(), "AIWorld.Enable":"1",
            "AIWorld.LivingRolesEnabled":"1", "AIWorld.LivingRoleExtensionsEnabled":"1",
            "AIWorld.LivingNeedEvolutionEnabled":"1", "AIWorld.LivingResidentsV11.Enabled":"1",
            "AIWorld.LivingSupplyEnabled":"0", "AIWorld.LivingConsumersEnabled":"0",
            "AIWorld.LivingFieldsEnabled":"0", "AIWorld.LivingRaidsEnabled":"0",
            "AIWorld.LivingDefias.Enabled":"0", "AIWorld.LivingCaravans.Enabled":"0",
            "AIWorld.NeedsHungerRatePerSecond":"0.003", "AIWorld.NeedsFatigueRatePerSecond":"0.001",
            "AIWorld.NeedsResourcePressureRatePerSecond":"0"}

    def observer_environment(self): return self.native_population.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA):
    population = Population.load(metadata, native.verify_data(data, metadata).geometry)
    return population


def snapshot_sql(population):
    sql = native.snapshot_sql(population.native_population)
    original = "SELECT JSON_OBJECT('kind','agent','id',agent_id)"
    replacement = "SELECT JSON_OBJECT('kind','agent','id',agent_id,'spawn',spawn_id,'map',map_id,'type',agent_type,'control',control_mode,'home_map',home_map_id,'home_x',home_x,'home_y',home_y,'home_z',home_z,'home_o',home_o,'faction',world_faction_id,'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)"
    require(sql.count(original) == 1, "V11 native agent read-back contract changed")
    sql = sql.replace(original, replacement)
    sql = sql.replace("JSON_OBJECT('kind','participation','spawn',spawn_id)",
                      "JSON_OBJECT('kind','participation','spawn',spawn_id,'mode',participation_mode)")
    return sql


def validate_snapshot(rows, population, *, bootstrap=False, control=1):
    require(control is None or type(control) is int and control in (0,1), "Invalid living control phase")
    require(isinstance(rows,list) and all(isinstance(r,dict) for r in rows), "Invalid living v11 read-back")
    actors = {a.spawn_id:a for a in population.actors}
    native_rows, agents, participation = [], set(), set()
    for row in rows:
        kind = row.get("kind")
        if kind == "agent":
            require(set(AGENT_FIELDS).issubset(row), "Incomplete living actor state")
            identity = row["id"]
            require(type(identity) is int and identity in actors and identity not in agents and
                all(type(row[k]) is int for k in ("spawn","map","type","control","home_map","faction")) and
                (row["spawn"],row["map"],row["type"],row["home_map"],row["faction"]) ==
                (identity,725,agent_type(identity),725,world_faction(identity)) and
                row["control"] in (0,1) and (bootstrap or control is None or row["control"] == control) and
                all(row[k] is None for k in WORK_FIELDS), "Living actor binding, role, faction or phase changed")
            actor = actors[identity]
            require(all(base.near(row[k],getattr(actor,field)) for k,field in
                        (("home_x","x"),("home_y","y"),("home_z","z"),("home_o","orientation"))),
                    "Living actor immutable home changed")
            agents.add(identity)
        elif kind == "participation":
            identity = row.get("spawn")
            require(type(identity) is int and identity in actors and identity not in participation and
                    type(row.get("mode")) is int and row["mode"] == 0,
                    "Living actor must have explicit unique FullAgent participation")
            participation.add(identity)
        elif kind == "memory":
            require(type(row.get("id")) is int and row["id"] in SPAWN_IDS,
                    "Retired or foreign memory owner in living world")
        else:
            native_rows.append(row)
    # The complete native world is mandatory even during the control migration.
    # Its validator still refuses foreign/retired spawns, groups and movement overrides.
    native.validate_snapshot(native_rows,population.native_population)
    if bootstrap:
        require((not agents and not participation) or agents == participation == SPAWN_IDS,
                "Living migration requires either native-only state or the complete reviewed control population")
    else:
        require(agents == participation == SPAWN_IDS, "Living v11 agent and participation bootstrap incomplete")


def _literal(value):
    return "'" + value.replace("'","''") + "'" if isinstance(value,str) else str(value)


def _spawn_predicate(actor, respawn_seconds):
    # Alias ownership is independently checked by character_templates before
    # entering this transaction; both its restored and installed form are valid.
    parts = [f"c.guid={actor.spawn_id}",f"c.id IN ({actor.entry},{native.expected_entry(actor.spawn_id)})",
        "c.map=725","c.zoneId=4988","c.areaId=4988","c.spawnMask=1","c.phaseMask=1",
        "c.MovementType=0","c.wander_distance=0",f"c.StringId={_literal(actor.label)}",
        "c.ScriptName=''",f"c.spawntimesecs={respawn_seconds}","c.npcflag=0","c.unit_flags=0","c.dynamicflags=0"]
    parts.extend(f"ABS(c.{column}-{getattr(actor,key)})<=0.001" for column,key in
        (("position_x","x"),("position_y","y"),("position_z","z"),("orientation","orientation")))
    columns={"entry":"entry","ai_name":"AIName","script_name":"ScriptName","regenerate_health":"RegenHealth","vehicle":"VehicleId"}
    source = native.DEFIAS_TEMPLATE if actor.spawn_id in DEFIAS_IDS else native.BASE_TEMPLATE
    parts.extend(f"ct.{columns.get(key,key)}={_literal(value)}" for key,value in source.items() if key != "entry")
    return "(" + " AND ".join(parts) + ")"


def _agent_predicate(actor):
    parts = [f"a.agent_id={actor.spawn_id}",f"a.spawn_id={actor.spawn_id}","a.map_id=725",
        f"a.agent_type={agent_type(actor.spawn_id)}",f"a.world_faction_id={world_faction(actor.spawn_id)}",
        "a.home_map_id=725","a.control_mode IN (0,1)"]
    parts.extend(f"ABS(a.{column}-{getattr(actor,key)})<=0.001" for column,key in
        (("home_x","x"),("home_y","y"),("home_z","z"),("home_o","orientation")))
    parts.extend(f"a.{key} IS NULL" for key in ("work_map_id","work_x","work_y","work_z","work_o"))
    return "(" + " AND ".join(parts) + ")"


def bootstrap_sql(population):
    """Guarded all-or-nothing control migration; never rewrites spawn homes."""
    require({a.spawn_id for a in population.actors} == SPAWN_IDS, "Incomplete living population")
    ids=population.spawn_ids
    reserved = lambda field: f"({field} BETWEEN 900725 AND 900824 OR {field} BETWEEN 901000 AND 901099 OR {field} BETWEEN 902000 AND 902044)"
    spawn_where=" OR ".join(_spawn_predicate(a,population.respawn_seconds) for a in population.actors)
    agent_where=" OR ".join(_agent_predicate(a) for a in population.actors)
    conditions=[
        f"(SELECT COUNT(*)=45 FROM world.creature c JOIN world.creature_template ct ON ct.entry=c.id WHERE {spawn_where})",
        f"(SELECT COUNT(*)=45 FROM world.creature WHERE map=725 OR {reserved('guid')} OR StringId LIKE 'aiworld_lab_resident_%')",
        f"(SELECT COUNT(*) IN (0,45) FROM characters.ai_agents WHERE map_id=725 OR {reserved('agent_id')} OR {reserved('spawn_id')} OR control_mode=1)",
        f"(SELECT COUNT(*)=(SELECT COUNT(*) FROM characters.ai_agents WHERE map_id=725 OR {reserved('agent_id')} OR {reserved('spawn_id')} OR control_mode=1) FROM characters.ai_agents a WHERE {agent_where})",
        f"(SELECT COUNT(*) IN (0,45) FROM world.ai_spawn_participation_defaults WHERE {reserved('spawn_id')})",
        f"(SELECT COUNT(*)=(SELECT COUNT(*) FROM world.ai_spawn_participation_defaults WHERE {reserved('spawn_id')}) FROM world.ai_spawn_participation_defaults WHERE spawn_id IN ({ids}) AND participation_mode=0)",
        f"(SELECT COUNT(*) FROM characters.ai_agents WHERE map_id=725)=(SELECT COUNT(*) FROM world.ai_spawn_participation_defaults WHERE {reserved('spawn_id')})",
        f"(SELECT COUNT(*)=0 FROM characters.ai_agent_group_members WHERE {reserved('member_agent_id')})",
        "(SELECT COUNT(*)=0 FROM characters.ai_long_term_memories WHERE agent_id BETWEEN 900725 AND 900824 OR agent_id BETWEEN 901000 AND 901099)",
        f"(SELECT COUNT(*)=0 FROM world.creature_addon WHERE guid IN ({ids}))",
        "(SELECT COUNT(*)=0 FROM world.creature_template_addon WHERE entry IN (38,11328) AND (path_id<>0 OR mount<>0 OR StandState<>0))",
        "(SELECT COUNT(*)=2 FROM world.ai_world_faction_entry_defaults WHERE (creature_entry=38 AND world_faction_id=2) OR (creature_entry=11328 AND world_faction_id=1))",
        "(SELECT COUNT(*)=45 FROM world.ai_world_faction_entry_defaults WHERE (creature_entry BETWEEN 9810000 AND 9810034 AND world_faction_id=1) OR (creature_entry BETWEEN 9810035 AND 9810044 AND world_faction_id=2))",
    ]
    template_columns={"entry":"entry","ai_name":"AIName","script_name":"ScriptName","regenerate_health":"RegenHealth","vehicle":"VehicleId"}
    for template in native.TEMPLATES:
        conditions.append("(SELECT COUNT(*)=1 FROM world.creature_template WHERE " + " AND ".join(
            f"{template_columns.get(key,key)}={_literal(value)}" for key,value in template.items()) + ")")
    script=native.DEFIAS_SMART_SCRIPTS[0]
    conditions.extend(("(SELECT COUNT(*)=1 FROM world.smart_scripts WHERE " + " AND ".join(
        f"`{key}`={_literal(script[key])}" for key in settlement.SMART_FIELDS) + ")",
        "(SELECT COUNT(*)=1 FROM world.smart_scripts WHERE source_type=0 AND (entryorguid IN (38,11328) OR entryorguid BETWEEN -902044 AND -902000))"))
    statements=["SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;", "START TRANSACTION;",
                "SET @living_v11_ready=" + " AND ".join(conditions) + ";"]
    for actor in population.actors:
        statements.append(f"INSERT INTO world.ai_spawn_participation_defaults (spawn_id,participation_mode) SELECT {actor.spawn_id},0 WHERE @living_v11_ready=1 AND NOT EXISTS(SELECT 1 FROM world.ai_spawn_participation_defaults WHERE spawn_id={actor.spawn_id});")
        statements.append(f"INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,home_map_id,home_x,home_y,home_z,home_o,world_faction_id) SELECT {actor.spawn_id},{agent_type(actor.spawn_id)},725,{actor.spawn_id},0,725,{actor.x},{actor.y},{actor.z},{actor.orientation},{world_faction(actor.spawn_id)} WHERE @living_v11_ready=1 AND NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE agent_id={actor.spawn_id} OR spawn_id={actor.spawn_id});")
    statements.append(f"UPDATE characters.ai_agents SET control_mode=1 WHERE @living_v11_ready=1 AND agent_id=spawn_id AND agent_id IN ({ids}) AND map_id=725 AND control_mode=0;")
    return "\n".join([*statements,"COMMIT;\n"])


def validate_telemetry(state, population):
    require(isinstance(state,dict) and state.get("version")==4 and state.get("stale") is False and
            type(state.get("captured_at_ms")) is int and state["captured_at_ms"] > 0, "Living v11 telemetry is stale")
    rows=state.get("agents")
    require(isinstance(rows,list) and len(rows)==45 and all(isinstance(r,dict) for r in rows) and
            {r.get("spawn_id") for r in rows}==SPAWN_IDS, "Living telemetry must contain the exact 45 authored residents")
    actors={a.spawn_id:a for a in population.actors}
    live_roles=0
    for row in rows:
        actor=actors[row["spawn_id"]]
        position,home=row.get("position"),row.get("home")
        role=row.get("living_role")
        needs=row.get("needs")
        require(row.get("native_creature") is False and row.get("control_mode")=="AI_WORLD_CONTROLLED" and
                row.get("agent_id")==actor.spawn_id and row.get("world_faction")==world_faction(actor.spawn_id) and
                row.get("entry")==actor.entry and row.get("template_entry")==native.expected_entry(actor.spawn_id) and
                row.get("type")==("COMBATANT" if actor.spawn_id in DEFIAS_IDS else "CIVILIAN") and
                isinstance(needs,dict) and all(type(needs.get(k)) in (int,float) and
                    math.isfinite(needs[k]) and 0<=needs[k]<=1 for k in ("hunger","fatigue")) and
                isinstance(row.get("economy"),dict) and row.get("llm_agent") is None and
                isinstance(position,dict) and position.get("map_id")==725 and
                all(type(position.get(k)) in (int,float) and math.isfinite(position[k]) for k in ("x","y","z")) and
                isinstance(home,dict) and home.get("map_id")==725 and
                all(base.near(home.get(k),getattr(actor,k)) for k in ("x","y","z")),
                "Living actor lost its controlled identity, native template, needs, role or immutable home")
        if row.get("alive") is True and position.get("source")=="live":
            require(isinstance(role,dict) and role.get("enabled") is True and
                    role.get("extensions_enabled") is True and role.get("role")==
                    ("RAIDER" if actor.spawn_id in DEFIAS_IDS else "WORKER") and
                    type(role.get("lifetime_ms")) is int and role["lifetime_ms"]>0 and
                    isinstance(row.get("runtime_guid"),str) and 0<len(row["runtime_guid"])<=80,
                    "Live resident role or incarnation is unavailable or misclassified")
            supplied=row.get("supply")
            expected_role={"farmer":"FARMER","village_consumer":"CONSUMER","city_consumer":"CONSUMER",
                           "city_carrier":"CARAVAN_CARRIER","raider":"RAIDER"}[capability(actor.spawn_id)]
            require(isinstance(supplied,dict) and supplied.get("enabled") is True and
                    supplied.get("role")==expected_role and supplied.get("depot_id")==depot_id(actor.spawn_id) and
                    isinstance(supplied.get("accounting"),dict) and supplied["accounting"].get("conserved") is True,
                    "Resident food capability, assigned depot or conserved ledger is unavailable")
            live_roles+=1
    return dict(total=45,managed=45,native=0,live_roles=live_roles,defias=10)
