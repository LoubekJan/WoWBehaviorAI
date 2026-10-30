"""Explicit AIWorld ownership of every authored preview-v8 NPC.

The remaining forty-five actors retain their authored identities and ordinary
types. Four form two physical food caravans and two city residents consume
the delivered food. Existing roles, service flags, templates, authored homes
and runtime-only ledgers stay unchanged. Character cards are a separate opt-in.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import defias_population as defias, raid_population as raids, settlement_population as settlement, single_return as base
except ImportError:
    import defias_population as defias
    import raid_population as raids
    import settlement_population as settlement
    import single_return as base

PROFILE = "full-v8"
METADATA = settlement.METADATA
MANIFEST = "full-population.json"
BASE_DEFIAS_SHA256 = "71bb975d39af18e4395a2acdd652cd10bda2d5166455d3c845800a3ea7a29a2a"
SPAWN_IDS = OWNED_IDS = settlement.SPAWN_IDS
NATIVE_IDS = frozenset()
RECRUIT_IDS = SPAWN_IDS - defias.SPAWN_IDS
GUARD_IDS = frozenset((*range(901000, 901008), 901020, 901021, 901026, 901027,
                       901032, 901033, 901038, 901042, 901043, 901065, 901070, 901071, 901097))
SERVICE_IDS = frozenset((*range(901008, 901014), 901040))
CIVILIAN_IDS = frozenset((901014, 901015, 901037, 901041))
COMBATANT_IDS = RECRUIT_IDS - GUARD_IDS - SERVICE_IDS - CIVILIAN_IDS
RECRUIT_TYPES = {identity: 1 if identity in GUARD_IDS else 2 if identity in SERVICE_IDS
                 else 0 if identity in CIVILIAN_IDS else 5 for identity in RECRUIT_IDS}
RECRUIT_FACTIONS = {identity: 1 if identity <= 901043 else 3 if identity <= 901065
                    else 4 if identity <= 901071 else 5 for identity in RECRUIT_IDS}
# These are actual LivingRolePolicy.Resolve results, separate from authored
# labels (resident, driver, fisher, miner...). All new civilians use entry11328.
RECRUIT_ROLES = {identity: {0: "WORKER", 1: "GUARD", 2: "SERVICE", 5: "COMBATANT"}[kind]
                 for identity, kind in RECRUIT_TYPES.items()}
CARAVAN_PAIRS = (
    {"id": 1, "carrier_spawn_id": 901040, "escort_spawn_id": 901042, "source_depot_ids": [4, 5]},
    {"id": 2, "carrier_spawn_id": 901041, "escort_spawn_id": 901043, "source_depot_ids": [6, 1]},
)
CARRIER_IDS = frozenset(pair["carrier_spawn_id"] for pair in CARAVAN_PAIRS)
ESCORT_IDS = frozenset(pair["escort_spawn_id"] for pair in CARAVAN_PAIRS)
CITY_CONSUMER_IDS = frozenset((901014, 901015))
CARAVAN_IDS = CARRIER_IDS | ESCORT_IDS
SUPPLY_RECRUIT_IDS = CARAVAN_IDS | CITY_CONSUMER_IDS
RECRUIT_CAPABILITIES = {identity: "caravan_carrier" if identity in CARRIER_IDS else
    "caravan_escort" if identity in ESCORT_IDS else "city_consumer" if identity in CITY_CONSUMER_IDS else
    "ordinary_living_role" for identity in RECRUIT_IDS}
RECRUIT_ROLES.update({identity: "CARAVAN_CARRIER" if identity in CARRIER_IDS else
    "CARAVAN_ESCORT" if identity in ESCORT_IDS else "CITY_CONSUMER" for identity in SUPPLY_RECRUIT_IDS})
CITY_DEPOT = {"id": 7, "name": "Stormwind city", "world_faction_id": 1,
    "position": {"map_id": 725, "x": 78.84711682419382, "y": 591.7054193209677, "z": .15},
    "capacity": 120, "target_stock": 60, "low_threshold": 12, "initial_stock": 0,
    "source_spawn_id": 901000}
WORK_FIELDS = defias.WORK_FIELDS
FIELDS = {**defias.FIELDS, "full_work": WORK_FIELDS}
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
    world_faction: int


def manifest_document(population: defias.Population) -> dict:
    actors = {actor.spawn_id: actor for actor in population.settlement.actors}
    faction_ids = {"stormwind": 1, "defias": 2, "gnolls": 3, "murlocs": 4, "kobolds": 5, "wildlife": 0}
    require(len(RECRUIT_IDS) == 45 and GUARD_IDS | SERVICE_IDS | CIVILIAN_IDS | COMBATANT_IDS == RECRUIT_IDS,
            "Full ownership must reserve exactly the remaining reviewed actors")
    require(all(RECRUIT_FACTIONS[identity] == faction_ids[actors[identity].faction] for identity in RECRUIT_IDS),
            "Full ownership must preserve authored factions")
    return dict(schema_version=1, profile=PROFILE, realm_id=2, map_id=725, area_id=4988,
        source_revision="preview-v8", defias_manifest_sha256=BASE_DEFIAS_SHA256,
        native_geometry_files_verified=61, owned_spawn_ids=sorted(SPAWN_IDS), native_spawn_ids=[],
        recruit_spawn_ids=sorted(RECRUIT_IDS), immutable_spawn_homes=True,
        runtime_food_state_only=True, runtime_raid_memory_only=True, runtime_raid_groups_only=True,
        runtime_caravans_only=True, llm_cards_explicit_opt_in=True, ordinary_roles_only=False,
        caravans={"enabled_config": "AIWorld.LivingCaravans.Enabled", "pairs": CARAVAN_PAIRS,
            "city_depot": CITY_DEPOT, "city_consumer_spawn_ids": sorted(CITY_CONSUMER_IDS),
            "source_depot_ids": [1, 4, 5, 6], "carrier_capacity": 6, "source_reserve": 4,
            "load_work_ms": 5000, "unload_work_ms": 5000, "rest_ms": 45000,
            "immutable_homes": True, "physical_food_only": True},
        actors=[dict(spawn_id=identity, entry=actors[identity].entry,
            label=actors[identity].label, role=actors[identity].role,
            living_role=RECRUIT_ROLES[identity], agent_type=RECRUIT_TYPES[identity],
            world_faction_id=RECRUIT_FACTIONS[identity], capability=RECRUIT_CAPABILITIES[identity],
            home=dict(x=actors[identity].x, y=actors[identity].y, z=actors[identity].z, o=actors[identity].orientation))
            for identity in sorted(RECRUIT_IDS)])


@dataclass(frozen=True)
class Population:
    defias_population: defias.Population
    recruits: tuple[Recruit, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        try:
            require(hashlib.sha256((metadata / defias.MANIFEST).read_bytes()).hexdigest() == BASE_DEFIAS_SHA256,
                    "Full ownership requires the unchanged reviewed Defias manifest")
        except OSError as exc:
            raise ProfileError("Missing reviewed full-ownership Defias baseline") from exc
        population = defias.Population.load(metadata)
        document = base.load_json(metadata / MANIFEST)
        require(settlement.digest(document) == settlement.digest(manifest_document(population)),
                "Full identities, roles, caravan capabilities, factions or immutable homes changed")
        originals = {actor.spawn_id: actor for actor in population.settlement.actors}
        recruits = tuple(Recruit(identity, originals[identity].entry, originals[identity].label,
            originals[identity].x, originals[identity].y, originals[identity].z, originals[identity].orientation,
            originals[identity].role, RECRUIT_TYPES[identity], RECRUIT_FACTIONS[identity]) for identity in sorted(RECRUIT_IDS))
        return cls(population, recruits)

    @property
    def settlement(self): return self.defias_population.settlement
    @property
    def field_population(self): return self.defias_population.field_population
    @property
    def wildlife(self): return self.defias_population.wildlife
    @property
    def supply(self): return self.defias_population.supply
    @property
    def consumers(self): return self.defias_population.consumers
    @property
    def raids(self): return self.defias_population.raids
    @property
    def fields(self): return self.defias_population.fields
    @property
    def actors(self): return self.defias_population.actors + self.recruits
    @property
    def spawn_ids(self): return ",".join(str(identity) for identity in sorted(SPAWN_IDS))
    @property
    def geometry_files(self): return self.settlement.geometry_files
    @property
    def respawn_seconds(self): return self.settlement.respawn_seconds

    def config(self) -> dict[str, str]:
        return {**self.defias_population.config(), "AIWorld.LivingCaravans.Enabled": "1"}

    def observer_environment(self) -> dict[str, str]:
        return self.settlement.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(defias.verify_data(data, metadata) == population.defias_population,
            "Full ownership geometry differs from the reviewed Defias baseline")
    return population


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(identity) for identity in sorted(RECRUIT_IDS))
    return defias.snapshot_sql(population.defias_population) + f"""
SELECT JSON_OBJECT('kind','full_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({ids}) OR spawn_id IN ({ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1, bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)), "Full ownership requires explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
            set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete full-ownership SQL read-back")
    recruits = {actor.spawn_id: actor for actor in population.recruits}
    seen = {kind: set() for kind in ("agent", "participation", "full_work")}
    retained = []
    for row in rows:
        kind = row["kind"]
        identities = {"agent": ("id", "spawn", "map", "type", "control", "faction"),
                      "participation": ("spawn",), "full_work": ("id", "spawn")}.get(kind, ())
        require(all(type(row[key]) is int and row[key] >= 0 for key in identities), "Invalid full-ownership SQL identity")
        if kind == "agent" and (row["id"] in RECRUIT_IDS or row["spawn"] in RECRUIT_IDS):
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind],
                    "Full agent/spawn collision or duplicate")
            actor = recruits[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                    (725, actor.agent_type, 725, None, actor.world_faction) and
                    all(base.near(row[key], expected) for key, expected in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                    "Full actor ordinary type, faction, workplace or immutable home changed")
            require(row["control"] in (0, 1) and (control is None or row["control"] == control), "Full control phase changed")
            seen[kind].add(identity)
        elif kind == "participation" and row["spawn"] in RECRUIT_IDS:
            identity = row["spawn"]
            require(identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0,
                    "Full participation must remain unique FullAgent")
            seen[kind].add(identity)
        elif kind == "full_work":
            identity = row["id"]
            require(identity in recruits and row["spawn"] == identity and identity not in seen[kind] and
                    all(row[key] is None for key in WORK_FIELDS if key not in ("id", "spawn")),
                    "Full ordinary workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in RECRUIT_IDS),
                    "Full ordinary actors must not acquire persisted group memberships")
            retained.append(row)
    require(seen["full_work"] == seen["agent"], "Full workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == RECRUIT_IDS and seen["participation"] == RECRUIT_IDS, "Full bootstrap incomplete")
    defias.validate_snapshot(retained, population.defias_population, control=control, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    return raids.bootstrap_sql(population)


def activation_sql(population: Population) -> str:
    bindings = []
    for actor in population.recruits:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND a.world_faction_id={actor.world_faction}
 AND c.id={actor.entry} AND c.StringId='{actor.label}'
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
    ready = f"""(SELECT COUNT(*)=45 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.control_mode IN (0,1) AND p.participation_mode=0
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND ({' OR '.join(bindings)})
 AND ({' OR '.join(templates)}))"""
    sql = defias.activation_sql(population.defias_population).replace(
        f"IN ({population.defias_population.spawn_ids}", f"IN ({population.spawn_ids}").replace(
        "@defias_population_ready", "@full_population_ready")
    marker = ";\nUPDATE characters.ai_agents"
    require(sql.count(marker) == 1, "Defias activation SQL layout changed")
    return sql.replace(marker, f" AND {ready}" + marker)


def deactivation_sql(population: Population) -> str:
    return raids.deactivation_sql(population)
