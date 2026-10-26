"""Opt-in three-hunter supply overlay over the unchanged v8 wildlife world.

Food, cargo and depot balances are runtime-only. Persistent writes add only
explicit spawn identities and control reservations; no entry-wide humanoid
classification, template, home, character or memory change is permitted.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import settlement_population as settlement, single_return as base, wildlife_population as wildlife
except ImportError:
    import settlement_population as settlement
    import single_return as base
    import wildlife_population as wildlife

PROFILE = "supply-v8"
METADATA = settlement.METADATA
MANIFEST = "supply-population.json"
HUNTER_IDS = frozenset((901034, 901061, 901062))
SPAWN_IDS = OWNED_IDS = wildlife.SPAWN_IDS | HUNTER_IDS
NATIVE_IDS = settlement.SPAWN_IDS - SPAWN_IDS
HUNTER_TYPE = 5  # AgentType::Combatant; retired value 3 must never be reused.
HUNTER_FACTIONS = {901034: 1, 901061: 3, 901062: 3}
BASE_WILDLIFE_SHA256 = "8d90827cc6b4bd6b295b7d29164ea5540af16cc5a52f53d6423664648f29a9cb"
FIELDS = {**wildlife.FIELDS, "supply_work": wildlife.WORK_FIELDS}
ProfileError, require, parse_snapshot = base.ProfileError, base.require, base.parse_snapshot

# These are authored anchors, not proof of a traversable route. Runtime must
# validate every route on the unchanged map; the normal homes remain immutable.
DEPOTS = (
    {"id": 1, "name": "villageBottom", "world_faction_id": 1,
     "position": {"map_id": 725, "x": -372.4611532703225, "y": 492.68216772838707, "z": 0.15}},
    {"id": 2, "name": "gnolls", "world_faction_id": 3,
     "position": {"map_id": 725, "x": 463.99765625, "y": 972.5791015625, "z": 13.096685538770844}},
)
GROUNDS = (
    {"id": 1, "name": "wildSouth", "radius_yards": 48,
     "position": {"map_id": 725, "x": -260.0, "y": 340.0, "z": 0.32227310920645647}},
    {"id": 2, "name": "wildLeft", "radius_yards": 48,
     "position": {"map_id": 725, "x": 245.0, "y": 1020.0, "z": 10.502432064212549}},
)


@dataclass(frozen=True)
class Hunter:
    spawn_id: int
    entry: int
    label: str
    x: float
    y: float
    z: float
    orientation: float
    world_faction: int
    depot_id: int
    ground_id: int
    role: str = "hunter"
    agent_type: int = HUNTER_TYPE


@dataclass(frozen=True)
class Population:
    wildlife: wildlife.Population
    hunters: tuple[Hunter, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        try:
            require(hashlib.sha256((metadata / wildlife.MANIFEST).read_bytes()).hexdigest() == BASE_WILDLIFE_SHA256,
                    "Supply requires the unchanged reviewed wildlife manifest")
        except OSError as exc:
            raise ProfileError("Missing reviewed supply wildlife baseline") from exc
        population = wildlife.Population.load(metadata)
        document = base.load_json(metadata / MANIFEST)
        require((document.get("schema_version"), document.get("profile"), document.get("realm_id"),
                 document.get("map_id"), document.get("area_id"), document.get("source_revision")) ==
                (1, PROFILE, 2, 725, 4988, "preview-v8"), "Supply profile/realm/map/source mismatch")
        require(document.get("wildlife_manifest_sha256") == BASE_WILDLIFE_SHA256 and
                document.get("settlement_manifest_sha256") == wildlife.BASE_MANIFEST_SHA256 and
                document.get("placement_proof_sha256") == wildlife.BASE_PLACEMENT_PROOF_SHA256,
                "Supply metadata does not pin the reviewed baseline")
        require(document.get("owned_spawn_ids") == sorted(SPAWN_IDS) and
                document.get("native_spawn_ids") == sorted(NATIVE_IDS) and
                document.get("hunters_spawn_ids") == sorted(HUNTER_IDS) and
                document.get("depots") == list(DEPOTS) and document.get("hunting_grounds") == list(GROUNDS) and
                document.get("runtime_food_state_only") is True,
                "Supply ownership, runtime anchors or restart policy changed")
        rows = document.get("hunters")
        require(isinstance(rows, list) and len(rows) == 3, "Supply requires exactly three reviewed hunters")
        expected = {actor.spawn_id: actor for actor in population.settlement.actors}
        hunters, seen = [], set()
        try:
            for row in rows:
                identity = row["spawn_id"]
                require(type(identity) is int and identity in HUNTER_IDS and identity not in seen,
                        "Duplicate/foreign supply hunter identity")
                seen.add(identity)
                actor = expected[identity]
                depot = 1 if identity == 901034 else 2
                home = tuple(base.finite(row["home"][field]) for field in ("x", "y", "z", "o"))
                require(row["entry"] == actor.entry and row["label"] == actor.label and
                        row["role"] == "hunter" and row["agent_type"] == HUNTER_TYPE and
                        row["world_faction_id"] == HUNTER_FACTIONS[identity] and
                        row["depot_id"] == depot and row["ground_id"] == depot and
                        home == (actor.x, actor.y, actor.z, actor.orientation),
                        "Supply hunter must preserve its native identity/home and explicit capability")
                hunters.append(Hunter(identity, actor.entry, actor.label, *home,
                                      HUNTER_FACTIONS[identity], depot, depot))
        except (KeyError, TypeError) as exc:
            raise ProfileError("Incomplete supply hunter metadata") from exc
        require(seen == HUNTER_IDS, "Supply hunter whitelist incomplete")
        return cls(population, tuple(sorted(hunters, key=lambda actor: actor.spawn_id)))

    @property
    def settlement(self):
        return self.wildlife.settlement

    @property
    def actors(self):
        return self.wildlife.actors + self.hunters

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
        result = self.wildlife.config()
        result["AIWorld.LivingSupplyEnabled"] = "1"
        return result

    def observer_environment(self) -> dict[str, str]:
        return self.settlement.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(wildlife.verify_data(data, metadata) == population.wildlife,
            "Supply geometry differs from the reviewed wildlife baseline")
    return population


def snapshot_sql(population: Population) -> str:
    ids = ",".join(str(identity) for identity in sorted(HUNTER_IDS))
    return wildlife.snapshot_sql(population.wildlife) + f"""
SELECT JSON_OBJECT('kind','supply_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({ids}) OR spawn_id IN ({ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1,
                      bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)),
            "Supply requires a valid explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
                set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete supply SQL read-back")
    identity_fields = {"spawn": ("guid",), "agent": ("id", "spawn", "map", "type", "control", "faction"),
                       "participation": ("spawn",), "membership": ("group", "member"),
                       "type": ("entry",), "wildlife_work": ("id", "spawn"), "supply_work": ("id", "spawn")}
    require(all(type(row[field]) is int and row[field] >= 0 for row in rows
                for field in identity_fields.get(row["kind"], ())), "Invalid supply SQL identity")
    hunters = {actor.spawn_id: actor for actor in population.hunters}
    seen = {kind: set() for kind in ("agent", "participation", "supply_work")}
    retained = []
    for row in rows:
        kind = row["kind"]
        if kind == "agent" and (row["id"] in HUNTER_IDS or row["spawn"] in HUNTER_IDS):
            identity = row["id"]
            require(type(identity) is int and type(row["spawn"]) is int and identity in hunters and
                    row["spawn"] == identity and identity not in seen[kind],
                    "Supply hunter agent/spawn collision or duplicate")
            actor = hunters[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                    (725, HUNTER_TYPE, 725, None, actor.world_faction), "Supply hunter map/type/work/faction drift")
            require(all(base.near(row[field], value) for field, value in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                    "Supply immutable hunter home changed")
            require(type(row["control"]) is int and row["control"] in (0, 1) and
                    (control is None or row["control"] == control), "Supply hunter control phase changed")
            seen[kind].add(identity)
        elif kind == "participation" and row["spawn"] in HUNTER_IDS:
            identity = row["spawn"]
            require(type(identity) is int and identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0,
                    "Supply hunter participation must be unique FullAgent")
            seen[kind].add(identity)
        elif kind == "supply_work":
            identity = row["id"]
            require(type(identity) is int and type(row["spawn"]) is int and identity in hunters and
                    row["spawn"] == identity and identity not in seen[kind] and
                    all(row[field] is None for field in wildlife.WORK_FIELDS if field not in ("id", "spawn")),
                    "Supply hunter workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in HUNTER_IDS),
                    "Supply hunter must not have group memberships")
            retained.append(row)
    require(seen["supply_work"] == seen["agent"], "Supply hunter workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == HUNTER_IDS and seen["participation"] == HUNTER_IDS,
                "Supply hunter bootstrap incomplete")
    wildlife.validate_snapshot(retained, population.wildlife, control=control, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    statements = ["START TRANSACTION;"]
    for actor in population.hunters:
        statements.append(f"""INSERT INTO world.ai_spawn_participation_defaults (spawn_id,participation_mode)
 SELECT {actor.spawn_id},0 WHERE NOT EXISTS(SELECT 1 FROM world.ai_spawn_participation_defaults WHERE spawn_id={actor.spawn_id});
INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,
 home_map_id,home_x,home_y,home_z,home_o,world_faction_id)
 SELECT {actor.spawn_id},{HUNTER_TYPE},725,{actor.spawn_id},0,725,
 {actor.x},{actor.y},{actor.z},{actor.orientation},{actor.world_faction}
 WHERE NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE agent_id={actor.spawn_id} OR spawn_id={actor.spawn_id});""")
    statements.append("COMMIT;")
    return "\n".join(statements) + "\n"


def activation_sql(population: Population) -> str:
    """One narrow activation with the original wildlife/native guards intact."""
    bindings = []
    for actor in population.hunters:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.world_faction_id={actor.world_faction} AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    columns = {"entry": "entry", "ai_name": "AIName", "script_name": "ScriptName",
               "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}
    templates = []
    for template in population.settlement.templates:
        if template["entry"] in {actor.entry for actor in population.hunters}:
            fields = []
            for field in settlement.TEMPLATE_FIELDS:
                value = template[field]
                literal = "'" + value.replace("'", "''") + "'" if isinstance(value, str) else str(value)
                fields.append(f"ct.{columns.get(field, field)}={literal}")
            templates.append("(" + " AND ".join(fields) + ")")
    hunter_ready = f"""(SELECT COUNT(*)=3 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.agent_type={HUNTER_TYPE}
 AND a.control_mode IN (0,1) AND p.participation_mode=0
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND ({' OR '.join(bindings)})
 AND ({' OR '.join(templates)}))"""
    # The wildlife generator already protects all 100 physical spawns and
    # the masked regression population. Extend only its ownership whitelist,
    # then require the three independently checked humanoid bindings as well.
    sql = wildlife.activation_sql(population.wildlife).replace(
        f"IN ({population.wildlife.spawn_ids}", f"IN ({population.spawn_ids}").replace(
        "@wildlife_population_ready", "@supply_population_ready")
    marker = ";\nUPDATE characters.ai_agents"
    require(sql.count(marker) == 1, "Wildlife activation SQL layout changed")
    return sql.replace(marker, f" AND {hunter_ready}" + marker)


def deactivation_sql(population: Population) -> str:
    return f"""START TRANSACTION;
UPDATE characters.ai_agents SET control_mode=0
 WHERE agent_id=spawn_id AND agent_id IN ({population.spawn_ids}) AND map_id=725 AND control_mode=1;
COMMIT;
"""
