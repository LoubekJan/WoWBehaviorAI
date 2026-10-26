"""Three ordinary depot consumers over the unchanged reviewed supply cohort.

The additional actors retain their civilian/combatant types and native homes.
Only their explicit control reservations are added. Food and consumer actions
remain runtime state; no economy, memory, template or map migration occurs.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import settlement_population as settlement, single_return as base, supply_population as supply
except ImportError:
    import settlement_population as settlement
    import single_return as base
    import supply_population as supply

PROFILE = "consumers-v8"
METADATA = settlement.METADATA
MANIFEST = "consumer-population.json"
CONSUMER_IDS = frozenset((901035, 901036, 901063))
CONSUMER_TYPES = {901035: 0, 901036: 0, 901063: 5}
CONSUMER_FACTIONS = {901035: 1, 901036: 1, 901063: 3}
CONSUMER_ROLES = {901035: "WORKER", 901036: "WORKER", 901063: "COMBATANT"}
CONSUMER_DEPOTS = {901035: 1, 901036: 1, 901063: 2}
SPAWN_IDS = OWNED_IDS = supply.SPAWN_IDS | CONSUMER_IDS
NATIVE_IDS = settlement.SPAWN_IDS - SPAWN_IDS
BASE_SUPPLY_SHA256 = "27996c621ff7c23b42e2fe7b1d0af0caf7e1ba760b79c9361947036bb8cf3f3a"
FIELDS = {**supply.FIELDS, "consumer_work": supply.wildlife.WORK_FIELDS}
ProfileError, require, parse_snapshot = base.ProfileError, base.require, base.parse_snapshot


@dataclass(frozen=True)
class Consumer:
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
    supply: supply.Population
    consumers: tuple[Consumer, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        try:
            require(hashlib.sha256((metadata / supply.MANIFEST).read_bytes()).hexdigest() == BASE_SUPPLY_SHA256,
                    "Consumers require the unchanged reviewed supply manifest")
        except OSError as exc:
            raise ProfileError("Missing reviewed consumer supply baseline") from exc
        population = supply.Population.load(metadata)
        document = base.load_json(metadata / MANIFEST)
        require((document.get("schema_version"), document.get("profile"), document.get("realm_id"),
                 document.get("map_id"), document.get("area_id"), document.get("source_revision")) ==
                (1, PROFILE, 2, 725, 4988, "preview-v8"), "Consumer profile/realm/map/source mismatch")
        require(document.get("supply_manifest_sha256") == BASE_SUPPLY_SHA256 and
                document.get("wildlife_manifest_sha256") == supply.BASE_WILDLIFE_SHA256 and
                document.get("settlement_manifest_sha256") == supply.wildlife.BASE_MANIFEST_SHA256 and
                document.get("placement_proof_sha256") == supply.wildlife.BASE_PLACEMENT_PROOF_SHA256,
                "Consumer metadata does not pin the reviewed baseline")
        require(document.get("owned_spawn_ids") == sorted(SPAWN_IDS) and
                document.get("native_spawn_ids") == sorted(NATIVE_IDS) and
                document.get("consumer_spawn_ids") == sorted(CONSUMER_IDS) and
                document.get("runtime_food_state_only") is True,
                "Consumer ownership or restart policy changed")
        rows = document.get("consumers")
        require(isinstance(rows, list) and len(rows) == 3, "Consumers require exactly three reviewed ordinary actors")
        expected = {actor.spawn_id: actor for actor in population.settlement.actors}
        consumers, seen = [], set()
        try:
            for row in rows:
                identity = row["spawn_id"]
                require(type(identity) is int and identity in CONSUMER_IDS and identity not in seen,
                        "Duplicate/foreign consumer identity")
                seen.add(identity)
                actor = expected[identity]
                home = tuple(base.finite(row["home"][field]) for field in ("x", "y", "z", "o"))
                require(row["entry"] == actor.entry and row["label"] == actor.label and row["role"] == actor.role and
                        row["capability"] == "consumer" and row["living_role"] == CONSUMER_ROLES[identity] and
                        row["agent_type"] == CONSUMER_TYPES[identity] and
                        row["world_faction_id"] == CONSUMER_FACTIONS[identity] and
                        row["depot_id"] == CONSUMER_DEPOTS[identity] and
                        home == (actor.x, actor.y, actor.z, actor.orientation),
                        "Consumer must preserve its ordinary type, faction, role and immutable home")
                consumers.append(Consumer(identity, actor.entry, actor.label, *home,
                    CONSUMER_TYPES[identity], CONSUMER_FACTIONS[identity], actor.role, CONSUMER_DEPOTS[identity]))
        except (KeyError, TypeError) as exc:
            raise ProfileError("Incomplete consumer metadata") from exc
        require(seen == CONSUMER_IDS, "Consumer whitelist incomplete")
        return cls(population, tuple(sorted(consumers, key=lambda actor: actor.spawn_id)))

    @property
    def settlement(self):
        return self.supply.settlement

    @property
    def wildlife(self):
        return self.supply.wildlife

    @property
    def actors(self):
        return self.supply.actors + self.consumers

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
        result = self.supply.config()
        result["AIWorld.LivingConsumersEnabled"] = "1"
        return result

    def observer_environment(self) -> dict[str, str]:
        return self.settlement.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(supply.verify_data(data, metadata) == population.supply,
            "Consumer geometry differs from the reviewed supply baseline")
    return population


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(identity) for identity in sorted(CONSUMER_IDS))
    return supply.snapshot_sql(population.supply) + f"""
SELECT JSON_OBJECT('kind','consumer_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({ids}) OR spawn_id IN ({ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1,
                      bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)),
            "Consumers require a valid explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
                set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete consumer SQL read-back")
    identity_fields = {"spawn": ("guid",), "agent": ("id", "spawn", "map", "type", "control", "faction"),
                       "participation": ("spawn",), "membership": ("group", "member"),
                       "type": ("entry",), "wildlife_work": ("id", "spawn"), "supply_work": ("id", "spawn"),
                       "consumer_work": ("id", "spawn")}
    require(all(type(row[field]) is int and row[field] >= 0 for row in rows
                for field in identity_fields.get(row["kind"], ())), "Invalid consumer SQL identity")
    consumers = {actor.spawn_id: actor for actor in population.consumers}
    seen = {kind: set() for kind in ("agent", "participation", "consumer_work")}
    retained = []
    for row in rows:
        kind = row["kind"]
        if kind == "agent" and (row["id"] in CONSUMER_IDS or row["spawn"] in CONSUMER_IDS):
            identity = row["id"]
            require(identity in consumers and row["spawn"] == identity and identity not in seen[kind],
                    "Consumer agent/spawn collision or duplicate")
            actor = consumers[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                    (725, actor.agent_type, 725, None, actor.world_faction), "Consumer ordinary map/type/work/faction drift")
            require(all(base.near(row[field], value) for field, value in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                    "Consumer immutable home changed")
            require(type(row["control"]) is int and row["control"] in (0, 1) and
                    (control is None or row["control"] == control), "Consumer control phase changed")
            seen[kind].add(identity)
        elif kind == "participation" and row["spawn"] in CONSUMER_IDS:
            identity = row["spawn"]
            require(identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0,
                    "Consumer participation must be unique FullAgent")
            seen[kind].add(identity)
        elif kind == "consumer_work":
            identity = row["id"]
            require(identity in consumers and row["spawn"] == identity and identity not in seen[kind] and
                    all(row[field] is None for field in supply.wildlife.WORK_FIELDS if field not in ("id", "spawn")),
                    "Consumer workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in CONSUMER_IDS),
                    "Consumer must not have group memberships")
            retained.append(row)
    require(seen["consumer_work"] == seen["agent"], "Consumer workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == CONSUMER_IDS and seen["participation"] == CONSUMER_IDS,
                "Consumer bootstrap incomplete")
    supply.validate_snapshot(retained, population.supply, control=control, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    statements = ["START TRANSACTION;"]
    for actor in population.consumers:
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
    """Extend the existing guarded activation without retyping ordinary actors."""
    bindings = []
    for actor in population.consumers:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND a.world_faction_id={actor.world_faction}
 AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    columns = {"entry": "entry", "ai_name": "AIName", "script_name": "ScriptName",
               "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}
    templates = []
    for template in population.settlement.templates:
        if template["entry"] in {actor.entry for actor in population.consumers}:
            fields = []
            for field in settlement.TEMPLATE_FIELDS:
                value = template[field]
                literal = "'" + value.replace("'", "''") + "'" if isinstance(value, str) else str(value)
                fields.append(f"ct.{columns.get(field, field)}={literal}")
            templates.append("(" + " AND ".join(fields) + ")")
    consumer_ready = f"""(SELECT COUNT(*)=3 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.control_mode IN (0,1) AND p.participation_mode=0
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND ({' OR '.join(bindings)})
 AND ({' OR '.join(templates)}))"""
    sql = supply.activation_sql(population.supply).replace(
        f"IN ({population.supply.spawn_ids}", f"IN ({population.spawn_ids}").replace(
        "@supply_population_ready", "@consumer_population_ready")
    marker = ";\nUPDATE characters.ai_agents"
    require(sql.count(marker) == 1, "Supply activation SQL layout changed")
    return sql.replace(marker, f" AND {consumer_ready}" + marker)


def deactivation_sql(population: Population) -> str:
    return f"""START TRANSACTION;
UPDATE characters.ai_agents SET control_mode=0
 WHERE agent_id=spawn_id AND agent_id IN ({population.spawn_ids}) AND map_id=725 AND control_mode=1;
COMMIT;
"""
