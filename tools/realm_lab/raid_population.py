"""Two explicitly reviewed raid actors over the unchanged consumer world.

Only identity reservations are persisted. Raids, food and cargo reset at restart;
native homes, ordinary agent types, templates and existing memories are retained.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import consumer_population as consumers, settlement_population as settlement, single_return as base
except ImportError:
    import consumer_population as consumers
    import settlement_population as settlement
    import single_return as base

PROFILE = "raids-v8"
METADATA = settlement.METADATA
MANIFEST = "raid-population.json"
RAIDER_IDS = frozenset((901055,))
GUARD_IDS = frozenset((901039,))
RECRUIT_IDS = RAIDER_IDS | GUARD_IDS
RECRUIT_TYPES = {901039: 1, 901055: 5}
RECRUIT_FACTIONS = {901039: 1, 901055: 2}
RECRUIT_ROLES = {901039: "GUARD", 901055: "RAIDER"}
RECRUIT_DEPOTS = {901039: 1, 901055: 3}
SPAWN_IDS = OWNED_IDS = consumers.SPAWN_IDS | RECRUIT_IDS
NATIVE_IDS = settlement.SPAWN_IDS - SPAWN_IDS
BASE_CONSUMERS_SHA256 = "338e7e9325883308de6c5c90bc04a52451d13c3da07c9478b40e27b9b778bc84"
MAX_HOME_DISTANCE_YARDS = 900
FIELDS = {**consumers.FIELDS, "raid_work": consumers.supply.wildlife.WORK_FIELDS}
ProfileError, require, parse_snapshot = base.ProfileError, base.require, base.parse_snapshot
DEPOT = {"id": 3, "name": "defias", "world_faction_id": 2,
         "position": {"map_id": 725, "x": 58.686744688134524, "y": 971.2687759381346, "z": 0.15}}
# These hints have complete native ground routes; physical floor/body checks
# still decide every runtime movement. The guard's ordinary home is separate.
GUARD_POST = consumers.supply.DEPOTS[0]["position"]
TRANSIT_WAYPOINTS = (
    {"map_id": 725, "x": -80.0, "y": 840.0, "z": 0.15},
    {"map_id": 725, "x": -235.0, "y": 725.0, "z": 0.15},
    {"map_id": 725, "x": -264.0, "y": 680.0, "z": 0.15},
    {"map_id": 725, "x": -340.0, "y": 580.0, "z": 0.15},
    GUARD_POST,
)


@dataclass(frozen=True)
class Recruit:
    spawn_id: int
    entry: int
    label: str
    x: float
    y: float
    z: float
    orientation: float
    agent_type: int
    world_faction: int
    role: str
    depot_id: int


@dataclass(frozen=True)
class Population:
    consumers: consumers.Population
    recruits: tuple[Recruit, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        try:
            require(hashlib.sha256((metadata / consumers.MANIFEST).read_bytes()).hexdigest() == BASE_CONSUMERS_SHA256,
                    "Raids require the unchanged reviewed consumer manifest")
        except OSError as exc:
            raise ProfileError("Missing reviewed raid consumer baseline") from exc
        population = consumers.Population.load(metadata)
        document = base.load_json(metadata / MANIFEST)
        require((document.get("schema_version"), document.get("profile"), document.get("realm_id"),
                 document.get("map_id"), document.get("area_id"), document.get("source_revision")) ==
                (1, PROFILE, 2, 725, 4988, "preview-v8"), "Raid profile/realm/map/source mismatch")
        require(document.get("consumer_manifest_sha256") == BASE_CONSUMERS_SHA256 and
                document.get("supply_manifest_sha256") == consumers.BASE_SUPPLY_SHA256 and
                document.get("wildlife_manifest_sha256") == consumers.supply.BASE_WILDLIFE_SHA256 and
                document.get("settlement_manifest_sha256") == consumers.supply.wildlife.BASE_MANIFEST_SHA256 and
                document.get("placement_proof_sha256") == consumers.supply.wildlife.BASE_PLACEMENT_PROOF_SHA256,
                "Raid metadata does not pin the reviewed baseline")
        require(document.get("owned_spawn_ids") == sorted(SPAWN_IDS) and
                document.get("native_spawn_ids") == sorted(NATIVE_IDS) and
                document.get("recruit_spawn_ids") == sorted(RECRUIT_IDS) and
                document.get("camp_depot") == DEPOT and type(document.get("source_depot_id")) is int and
                document["source_depot_id"] == 1 and document.get("guard_post") == GUARD_POST and
                document.get("transit_waypoints") == list(TRANSIT_WAYPOINTS) and
                type(document.get("raider_max_home_distance_yards")) is int and
                document["raider_max_home_distance_yards"] == MAX_HOME_DISTANCE_YARDS and
                document.get("runtime_food_state_only") is True,
                "Raid ownership, runtime anchors or restart policy changed")
        rows = document.get("recruits")
        require(isinstance(rows, list) and len(rows) == 2, "Raids require exactly two reviewed actors")
        expected = {actor.spawn_id: actor for actor in population.settlement.actors}
        recruits, seen = [], set()
        try:
            for row in rows:
                identity = row["spawn_id"]
                require(type(identity) is int and identity in RECRUIT_IDS and identity not in seen,
                        "Duplicate/foreign raid identity")
                seen.add(identity)
                actor = expected[identity]
                home = tuple(base.finite(row["home"][field]) for field in ("x", "y", "z", "o"))
                raider = identity in RAIDER_IDS
                require(all(type(row[field]) is int for field in
                            ("agent_type", "world_faction_id", "depot_id", "cargo_capacity")) and
                        row["entry"] == actor.entry and row["label"] == actor.label and row["role"] == actor.role and
                        row["capability"] == ("raider" if raider else "consumer") and
                        row["living_role"] == RECRUIT_ROLES[identity] and
                        row["agent_type"] == RECRUIT_TYPES[identity] and
                        row["world_faction_id"] == RECRUIT_FACTIONS[identity] and
                        row["depot_id"] == RECRUIT_DEPOTS[identity] and
                        row["cargo_capacity"] == (2 if raider else 0) and
                        home == (actor.x, actor.y, actor.z, actor.orientation),
                        "Raid actor must preserve ordinary type, faction, role, immutable home and reviewed capability")
                recruits.append(Recruit(identity, actor.entry, actor.label, *home,
                    RECRUIT_TYPES[identity], RECRUIT_FACTIONS[identity], actor.role, RECRUIT_DEPOTS[identity]))
        except (KeyError, TypeError) as exc:
            raise ProfileError("Incomplete raid metadata") from exc
        require(seen == RECRUIT_IDS, "Raid whitelist incomplete")
        return cls(population, tuple(sorted(recruits, key=lambda actor: actor.spawn_id)))

    @property
    def supply(self):
        return self.consumers.supply

    @property
    def settlement(self):
        return self.consumers.settlement

    @property
    def wildlife(self):
        return self.consumers.wildlife

    @property
    def actors(self):
        return self.consumers.actors + self.recruits

    @property
    def spawn_ids(self) -> str:
        return ",".join(str(identity) for identity in sorted(SPAWN_IDS))

    @property
    def geometry_files(self):
        return self.settlement.geometry_files

    @property
    def respawn_seconds(self):
        return self.settlement.respawn_seconds

    def config(self) -> dict[str, str]:
        result = self.consumers.config()
        result["AIWorld.LivingRaidsEnabled"] = "1"
        return result

    def observer_environment(self) -> dict[str, str]:
        return self.settlement.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(consumers.verify_data(data, metadata) == population.consumers,
            "Raid geometry differs from the reviewed consumer baseline")
    return population


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(identity) for identity in sorted(RECRUIT_IDS))
    return consumers.snapshot_sql(population.consumers) + f"""
SELECT JSON_OBJECT('kind','raid_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({ids}) OR spawn_id IN ({ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1,
                      bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)), "Raids require an explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
                set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete raid SQL read-back")
    recruits = {actor.spawn_id: actor for actor in population.recruits}
    seen = {kind: set() for kind in ("agent", "participation", "raid_work")}
    retained = []
    for row in rows:
        kind = row["kind"]
        fields = {"agent": ("id", "spawn", "map", "type", "control", "faction"),
                  "participation": ("spawn",), "raid_work": ("id", "spawn")}.get(kind, ())
        require(all(type(row[field]) is int and row[field] >= 0 for field in fields), "Invalid raid SQL identity")
        if kind == "agent" and (row["id"] in RECRUIT_IDS or row["spawn"] in RECRUIT_IDS):
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind],
                    "Raid agent/spawn collision or duplicate")
            actor = recruits[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                    (725, actor.agent_type, 725, None, actor.world_faction), "Raid ordinary map/type/work/faction drift")
            require(all(base.near(row[field], value) for field, value in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                    "Raid immutable home changed")
            require(row["control"] in (0, 1) and (control is None or row["control"] == control),
                    "Raid control phase changed")
            seen[kind].add(identity)
        elif kind == "participation" and row["spawn"] in RECRUIT_IDS:
            identity = row["spawn"]
            require(identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0,
                    "Raid participation must be unique FullAgent")
            seen[kind].add(identity)
        elif kind == "raid_work":
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind] and
                    all(row[field] is None for field in consumers.supply.wildlife.WORK_FIELDS if field not in ("id", "spawn")),
                    "Raid workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in RECRUIT_IDS), "Raid actor must not have group memberships")
            retained.append(row)
    require(seen["raid_work"] == seen["agent"], "Raid workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == RECRUIT_IDS and seen["participation"] == RECRUIT_IDS, "Raid bootstrap incomplete")
    consumers.validate_snapshot(retained, population.consumers, control=control, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    statements = ["START TRANSACTION;"]
    for actor in population.recruits:
        statements.append(f"""INSERT INTO world.ai_spawn_participation_defaults (spawn_id,participation_mode)
 SELECT {actor.spawn_id},0 WHERE NOT EXISTS(SELECT 1 FROM world.ai_spawn_participation_defaults WHERE spawn_id={actor.spawn_id});
INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,
 home_map_id,home_x,home_y,home_z,home_o,world_faction_id)
 SELECT {actor.spawn_id},{actor.agent_type},725,{actor.spawn_id},0,725,
 {actor.x},{actor.y},{actor.z},{actor.orientation},{actor.world_faction}
 WHERE NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE agent_id={actor.spawn_id} OR spawn_id={actor.spawn_id});""")
    statements.append("COMMIT;")
    return "\n".join(statements) + "\n"


def activation_sql(population: Population) -> str:
    bindings = []
    for actor in population.recruits:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND a.world_faction_id={actor.world_faction}
 AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    columns = {"entry": "entry", "ai_name": "AIName", "script_name": "ScriptName",
               "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}
    templates = []
    for template in population.settlement.templates:
        if template["entry"] in {actor.entry for actor in population.recruits}:
            fields = []
            for field in settlement.TEMPLATE_FIELDS:
                value = template[field]
                literal = "'" + value.replace("'", "''") + "'" if isinstance(value, str) else str(value)
                fields.append(f"ct.{columns.get(field, field)}={literal}")
            templates.append("(" + " AND ".join(fields) + ")")
    ready = f"""(SELECT COUNT(*)=2 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.control_mode IN (0,1) AND p.participation_mode=0
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND ({' OR '.join(bindings)})
 AND ({' OR '.join(templates)}))"""
    sql = consumers.activation_sql(population.consumers).replace(
        f"IN ({population.consumers.spawn_ids}", f"IN ({population.spawn_ids}").replace(
        "@consumer_population_ready", "@raid_population_ready")
    marker = ";\nUPDATE characters.ai_agents"
    require(sql.count(marker) == 1, "Consumer activation SQL layout changed")
    return sql.replace(marker, f" AND {ready}" + marker)


def deactivation_sql(population: Population) -> str:
    return f"""START TRANSACTION;
UPDATE characters.ai_agents SET control_mode=0
 WHERE agent_id=spawn_id AND agent_id IN ({population.spawn_ids}) AND map_id=725 AND control_mode=1;
COMMIT;
"""
