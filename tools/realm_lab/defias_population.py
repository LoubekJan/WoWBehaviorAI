"""Explicit control of the authored Defias camp over the unchanged field world.

The overlay adds fifteen identity reservations. All hundred spawn homes,
ordinary actor types, native templates and previous profile receipts stay fixed.
Food, remembered raid outcomes and raid parties remain runtime state.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import field_population as fields, raid_population as raids, settlement_population as settlement, single_return as base
except ImportError:
    import field_population as fields
    import raid_population as raids
    import settlement_population as settlement
    import single_return as base

PROFILE = "defias-v8"
METADATA = settlement.METADATA
MANIFEST = "defias-population.json"
BASE_FIELDS_SHA256 = "304188555819914d88307dd45d1b2fd6d57fc7995b6f457005948600ccbd2430"
DEFIAS_IDS = ALL_DEFIAS_IDS = frozenset(range(901044, 901060))
RAIDER_IDS = frozenset(range(901050, 901060))
GUARD_IDS = frozenset(range(901044, 901048))
SUPPLIER_IDS = frozenset((901048, 901049))
RECRUIT_IDS = DEFIAS_IDS - fields.SPAWN_IDS
RECRUIT_TYPES = {identity: 5 if identity in RAIDER_IDS else 1 if identity in GUARD_IDS else 0 for identity in DEFIAS_IDS}
RECRUIT_FACTIONS = {identity: 2 for identity in DEFIAS_IDS}
RECRUIT_ROLES = {identity: "RAIDER" if identity in RAIDER_IDS else "GUARD" if identity in GUARD_IDS else "WORKER" for identity in DEFIAS_IDS}
SPAWN_IDS = OWNED_IDS = fields.SPAWN_IDS | DEFIAS_IDS
NATIVE_IDS = settlement.SPAWN_IDS - SPAWN_IDS
WORK_FIELDS = fields.WORK_FIELDS
FIELDS = {**fields.FIELDS, "defias_work": WORK_FIELDS}
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
    agent_type: int
    world_faction: int = 2
    depot_id: int = 3


def manifest_document(population: fields.Population) -> dict:
    """Derive the only accepted overlay from the pinned authored identities."""
    actors = {actor.spawn_id: actor for actor in population.settlement.actors}
    require({actor.spawn_id for actor in population.settlement.actors if actor.faction == "defias"} == DEFIAS_IDS,
            "The authored Defias identities differ from the reviewed camp")
    return dict(schema_version=1, profile=PROFILE, realm_id=2, map_id=725, area_id=4988,
        source_revision="preview-v8", field_manifest_sha256=BASE_FIELDS_SHA256,
        native_geometry_files_verified=61, owned_spawn_ids=sorted(SPAWN_IDS),
        native_spawn_ids=sorted(NATIVE_IDS), defias_spawn_ids=sorted(DEFIAS_IDS),
        raider_spawn_ids=sorted(RAIDER_IDS), runtime_food_state_only=True,
        runtime_raid_memory_only=True, runtime_raid_groups_only=True,
        immutable_spawn_homes=True, camp_depot=raids.DEPOT, raider_cargo_capacity=2,
        actors=[dict(spawn_id=identity, entry=actors[identity].entry,
            label=actors[identity].label, role=actors[identity].role,
            living_role=RECRUIT_ROLES[identity], agent_type=RECRUIT_TYPES[identity],
            world_faction_id=2, depot_id=3, capability="raider" if identity in RAIDER_IDS else "consumer",
            home=dict(x=actors[identity].x, y=actors[identity].y, z=actors[identity].z, o=actors[identity].orientation))
            for identity in sorted(DEFIAS_IDS)])


@dataclass(frozen=True)
class Population:
    field_population: fields.Population
    recruits: tuple[Recruit, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        try:
            require(hashlib.sha256((metadata / fields.MANIFEST).read_bytes()).hexdigest() == BASE_FIELDS_SHA256,
                    "Defias requires the unchanged reviewed field manifest")
        except OSError as exc:
            raise ProfileError("Missing reviewed Defias field baseline") from exc
        population = fields.Population.load(metadata)
        document = base.load_json(metadata / MANIFEST)
        require(settlement.digest(document) == settlement.digest(manifest_document(population)),
                "Defias identities, ordinary types, runtime capabilities or immutable homes changed")
        originals = {actor.spawn_id: actor for actor in population.settlement.actors}
        recruits = tuple(Recruit(identity, originals[identity].entry, originals[identity].label,
            originals[identity].x, originals[identity].y, originals[identity].z, originals[identity].orientation,
            originals[identity].role, RECRUIT_TYPES[identity]) for identity in sorted(RECRUIT_IDS))
        return cls(population, recruits)

    @property
    def settlement(self): return self.field_population.settlement
    @property
    def wildlife(self): return self.field_population.wildlife
    @property
    def supply(self): return self.field_population.supply
    @property
    def consumers(self): return self.field_population.consumers
    @property
    def raids(self): return self.field_population.raids
    @property
    def fields(self): return self.field_population.fields
    @property
    def actors(self): return self.field_population.actors + self.recruits
    @property
    def spawn_ids(self): return ",".join(str(identity) for identity in sorted(SPAWN_IDS))
    @property
    def geometry_files(self): return self.settlement.geometry_files
    @property
    def respawn_seconds(self): return self.settlement.respawn_seconds

    def config(self) -> dict[str, str]:
        return {**self.field_population.config(), "AIWorld.LivingDefias.Enabled": "1"}

    def observer_environment(self) -> dict[str, str]:
        return self.settlement.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(fields.verify_data(data, metadata) == population.field_population,
            "Defias geometry differs from the reviewed field baseline")
    return population


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(identity) for identity in sorted(RECRUIT_IDS))
    return fields.snapshot_sql(population.field_population) + f"""
SELECT JSON_OBJECT('kind','defias_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({ids}) OR spawn_id IN ({ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1, bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)), "Defias requires explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
            set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete Defias SQL read-back")
    recruits = {actor.spawn_id: actor for actor in population.recruits}
    seen = {kind: set() for kind in ("agent", "participation", "defias_work")}
    retained = []
    for row in rows:
        kind = row["kind"]
        identities = {"agent": ("id", "spawn", "map", "type", "control", "faction"),
                      "participation": ("spawn",), "defias_work": ("id", "spawn")}.get(kind, ())
        require(all(type(row[key]) is int and row[key] >= 0 for key in identities), "Invalid Defias SQL identity")
        if kind == "agent" and (row["id"] in RECRUIT_IDS or row["spawn"] in RECRUIT_IDS):
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind],
                    "Defias agent/spawn collision or duplicate")
            actor = recruits[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                    (725, actor.agent_type, 725, None, 2) and
                    all(base.near(row[key], expected) for key, expected in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                    "Defias ordinary type, faction, workplace or immutable home changed")
            require(row["control"] in (0, 1) and (control is None or row["control"] == control), "Defias control phase changed")
            seen[kind].add(identity)
        elif kind == "participation" and row["spawn"] in RECRUIT_IDS:
            identity = row["spawn"]
            require(identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0,
                    "Defias participation must remain unique FullAgent")
            seen[kind].add(identity)
        elif kind == "defias_work":
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind] and
                    all(row[key] is None for key in WORK_FIELDS if key not in ("id", "spawn")),
                    "Defias workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in RECRUIT_IDS),
                    "Runtime Defias parties must not acquire persisted group memberships")
            retained.append(row)
    require(seen["defias_work"] == seen["agent"], "Defias workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == RECRUIT_IDS and seen["participation"] == RECRUIT_IDS, "Defias bootstrap incomplete")
    fields.validate_snapshot(retained, population.field_population, control=control, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    return raids.bootstrap_sql(population)


def activation_sql(population: Population) -> str:
    bindings = []
    for actor in population.recruits:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND a.world_faction_id=2 AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    columns = {"entry": "entry", "ai_name": "AIName", "script_name": "ScriptName", "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}
    templates = []
    for template in population.settlement.templates:
        if template["entry"] in {actor.entry for actor in population.recruits}:
            clauses = []
            for key in settlement.TEMPLATE_FIELDS:
                value = template[key]
                literal = "'" + value.replace("'", "''") + "'" if isinstance(value, str) else str(value)
                clauses.append(f"ct.{columns.get(key, key)}={literal}")
            templates.append("(" + " AND ".join(clauses) + ")")
    ready = f"""(SELECT COUNT(*)=15 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.control_mode IN (0,1) AND p.participation_mode=0
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND ({' OR '.join(bindings)})
 AND ({' OR '.join(templates)}))"""
    sql = fields.activation_sql(population.field_population).replace(
        f"IN ({population.field_population.spawn_ids}", f"IN ({population.spawn_ids}").replace(
        "@field_population_ready", "@defias_population_ready")
    marker = ";\nUPDATE characters.ai_agents"
    require(sql.count(marker) == 1, "Field activation SQL layout changed")
    return sql.replace(marker, f" AND {ready}" + marker)


def deactivation_sql(population: Population) -> str:
    return raids.deactivation_sql(population)
