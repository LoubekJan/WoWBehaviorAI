"""Opt-in wildlife ownership over the unchanged authored settlement.

Only the twenty reviewed wildlife acquire persistent AI bindings. The native
population, all homes, geometry and any complete masked regression population
remain subject to settlement's original validator. Writes require an offline
world and the deployment lock; callers must validate before and after SQL.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

try:
    from . import hunt_population as hunt, settlement_population as settlement, single_return as base
except ImportError:
    import hunt_population as hunt
    import settlement_population as settlement
    import single_return as base

PROFILE = "wildlife-v8"
METADATA = settlement.METADATA
MANIFEST = "wildlife-population.json"
SPAWN_IDS = OWNED_IDS = frozenset(range(901072, 901092))
PREDATOR_IDS = frozenset((901076, 901081, 901086, 901091))
PREY_IDS = SPAWN_IDS - PREDATOR_IDS
NATIVE_IDS = settlement.SPAWN_IDS - SPAWN_IDS
ROLE_TYPES = hunt.ROLE_TYPES
ROLE_ENTRIES = hunt.ROLE_ENTRIES
# Pin reviewed repository bytes, rather than allowing two edited manifests to
# redefine one another. These files have an explicit Git LF contract.
BASE_MANIFEST_SHA256 = "107489731df5d4860e10b9829b9730a55501ee2de8f61e5432f6cdf616bf0b8d"
BASE_PLACEMENT_PROOF_SHA256 = "bd940387800b0d710196fbef114847edbe221e682c36e3b3948d55767a511071"
NEED_RATES = {"hunger_per_second": 0.003, "fatigue_per_second": 0, "resource_pressure_per_second": 0}
WORK_FIELDS = "id spawn work_map work_x work_y work_z work_o".split()
FIELDS = {**settlement.FIELDS, "type": ["entry", "type"], "wildlife_work": WORK_FIELDS}
ProfileError = base.ProfileError
require = base.require
parse_snapshot = base.parse_snapshot


@dataclass(frozen=True)
class Population:
    settlement: settlement.Population
    actors: tuple[hunt.Actor, ...]
    profile: str = PROFILE

    @classmethod
    def load(cls, metadata: Path = METADATA) -> Population:
        try:
            require(hashlib.sha256((metadata / settlement.MANIFEST).read_bytes()).hexdigest() ==
                    BASE_MANIFEST_SHA256, "Wildlife requires the unchanged reviewed settlement manifest")
            require(hashlib.sha256((metadata / settlement.POSITION_PROOF).read_bytes()).hexdigest() ==
                    BASE_PLACEMENT_PROOF_SHA256, "Wildlife requires the unchanged native placement proof")
        except OSError as exc:
            raise ProfileError("Missing reviewed wildlife settlement baseline") from exc
        native = settlement.Population.load(metadata)
        document = base.load_json(metadata / MANIFEST)
        require((document.get("profile"), document.get("realm_id"), document.get("map_id"),
                 document.get("area_id"), document.get("source_revision")) ==
                (PROFILE, 2, 725, 4988, "preview-v8"), "Wildlife profile/realm/map/source mismatch")
        require(document.get("settlement_manifest_sha256") == BASE_MANIFEST_SHA256 and
                document.get("placement_proof_sha256") == BASE_PLACEMENT_PROOF_SHA256,
                "Wildlife metadata does not pin the reviewed native settlement")
        require(document.get("owned_spawn_ids") == sorted(SPAWN_IDS) and
                document.get("native_spawn_ids") == sorted(NATIVE_IDS) and
                document.get("role_counts") == {"predator": 4, "prey": 16} and
                document.get("need_rates") == NEED_RATES,
                "Wildlife ownership, role counts or natural need rates changed")
        rows = document.get("actors")
        require(isinstance(rows, list) and len(rows) == 20, "Wildlife requires exactly twenty reviewed actors")
        expected = {actor.spawn_id: actor for actor in native.actors}
        actors, seen = [], set()
        try:
            for row in rows:
                require(isinstance(row, dict), "Invalid wildlife actor metadata")
                identity = settlement.integer(row["spawn_id"], 901072, 901091, "wildlife spawn ID")
                require(identity not in seen and identity in SPAWN_IDS, "Duplicate/foreign wildlife identity")
                seen.add(identity)
                actor = expected[identity]
                role = "predator" if identity in PREDATOR_IDS else "prey"
                coordinates = tuple(base.finite(row["home"][field]) for field in ("x", "y", "z", "o"))
                require(actor.faction == "wildlife" and actor.role == role and
                        actor.entry == ROLE_ENTRIES[role] and row["role"] == role and
                        row["entry"] == actor.entry and row["label"] == actor.label and
                        coordinates == (actor.x, actor.y, actor.z, actor.orientation),
                        "Wildlife must preserve native entries, labels, roles and exact homes")
                actors.append(hunt.Actor(identity, actor.entry, role, actor.label, *coordinates))
        except (KeyError, TypeError) as exc:
            raise ProfileError("Incomplete wildlife actor metadata") from exc
        require(seen == SPAWN_IDS, "Wildlife ownership whitelist incomplete")
        return cls(native, tuple(sorted(actors, key=lambda actor: actor.spawn_id)))

    @property
    def spawn_ids(self) -> str:
        """Owned AI IDs; the observation/active map scope remains all 100."""
        return ",".join(str(actor.spawn_id) for actor in self.actors)

    @property
    def entries(self) -> str:
        return ",".join(str(entry) for entry in sorted(ROLE_ENTRIES.values()))

    @property
    def geometry_files(self) -> tuple[dict, ...]:
        return self.settlement.geometry_files

    @property
    def respawn_seconds(self) -> int:
        return self.settlement.respawn_seconds

    def config(self) -> dict[str, str]:
        result = self.settlement.config()
        result.update({
            "AIWorld.Enable": "1", "AIWorld.LivingRolesEnabled": "1",
            "AIWorld.LivingNeedEvolutionEnabled": "1", "AIWorld.LivingRoleExtensionsEnabled": "1",
            "AIWorld.TelemetryEnabled": "1", "AIWorld.TelemetryNativeCreaturesEnabled": "1",
            "AIWorld.EnableSpawnReconciliation": "0", "AIWorld.EnableZoneControlActivation": "0",
            "AIWorld.ElwynnAlwaysActive": "0", "AIWorld.LivingWolvesEnabled": "0",
            "AIWorld.WolfGroupAutoFormation": "0", "AIWorld.DefiasGroupAutoFormation": "0",
            "AIWorld.RemoteInferenceEnabled": "0", "AIWorld.GroupCoarseSimulationEnabled": "0",
            "AIWorld.CoalitionMaintenance": "0", "AIWorld.GroupCoordination": "0",
            "AIWorld.DynamicTaskEnable": "0", "AIWorld.RecoveryAdviceEnabled": "0",
            "AIWorld.NeedsHungerRatePerSecond": "0.003", "AIWorld.NeedsFatigueRatePerSecond": "0",
            "AIWorld.NeedsResourcePressureRatePerSecond": "0",
        })
        return result

    def observer_environment(self) -> dict[str, str]:
        return self.settlement.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA) -> Population:
    population = Population.load(metadata)
    require(settlement.verify_data(data, metadata) == population.settlement,
            "Wildlife geometry differs from the reviewed native settlement")
    return population


def snapshot_sql(population: Population) -> str:
    # Preserve the existing legacy JSON byte contract, including its fields.
    # Work coordinates are queried separately for owned actors only.
    return settlement.snapshot_sql(population.settlement) + f"""
SELECT JSON_OBJECT('kind','type','entry',creature_entry,'type',agent_type)
 FROM world.ai_agent_type_entry_defaults WHERE creature_entry IN ({population.entries});
SELECT JSON_OBJECT('kind','wildlife_work','id',agent_id,'spawn',spawn_id,
 'work_map',work_map_id,'work_x',work_x,'work_y',work_y,'work_z',work_z,'work_o',work_o)
 FROM characters.ai_agents WHERE agent_id IN ({population.spawn_ids}) OR spawn_id IN ({population.spawn_ids});
"""


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None = 1,
                      bootstrap: bool = False) -> None:
    require(control is None or (type(control) is int and control in (0, 1)),
            "Wildlife requires a valid explicit activation phase")
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get("kind") in FIELDS and
                set(FIELDS[row["kind"]]).issubset(row) for row in rows), "Incomplete wildlife SQL read-back")
    identity_fields = {"spawn": ("guid",), "agent": ("id", "spawn"), "participation": ("spawn",),
                       "membership": ("group", "member"), "type": ("entry",),
                       "wildlife_work": ("id", "spawn")}
    for row in rows:
        require(all(type(row[field]) is int and row[field] >= 0
                    for field in identity_fields.get(row["kind"], ())), "Invalid wildlife SQL identity")
    actors = {actor.spawn_id: actor for actor in population.actors}
    seen = {kind: set() for kind in ("agent", "participation", "type", "wildlife_work")}
    native_rows = []
    for row in rows:
        kind = row["kind"]
        owned_agent = kind == "agent" and (row["id"] in SPAWN_IDS or row["spawn"] in SPAWN_IDS)
        owned_participation = kind == "participation" and row["spawn"] in SPAWN_IDS
        if owned_agent:
            identity = row["id"]
            require(identity in actors and row["spawn"] == identity and identity not in seen[kind],
                    "Wildlife agent ID/spawn collision or duplicate")
            actor = actors[identity]
            require((row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                    (725, actor.agent_type, 725, None, 0), "Wildlife agent map/type/work/faction drift")
            require(all(base.near(row[field], value) for field, value in
                        (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z),
                         ("home_o", actor.orientation))), "Wildlife immutable home changed")
            require(type(row["control"]) is int and row["control"] in (0, 1) and
                    (control is None or row["control"] == control), "Wildlife control mode differs from activation phase")
            seen[kind].add(identity)
        elif owned_participation:
            identity = row["spawn"]
            require(identity not in seen[kind] and type(row["mode"]) is int and row["mode"] == 0,
                    "Wildlife participation must be unique FullAgent")
            seen[kind].add(identity)
        elif kind == "type":
            identity = row["entry"]
            expected = {entry: ROLE_TYPES[role] for role, entry in ROLE_ENTRIES.items()}
            require(identity in expected and identity not in seen[kind] and
                    type(row["type"]) is int and row["type"] == expected[identity],
                    "Wildlife entry classification mismatch")
            seen[kind].add(identity)
        elif kind == "wildlife_work":
            identity = row["id"]
            require(identity in actors and row["spawn"] == identity and identity not in seen[kind] and
                    all(row[field] is None for field in WORK_FIELDS if field not in ("id", "spawn")),
                    "Wildlife workplace must remain wholly absent")
            seen[kind].add(identity)
        else:
            require(not (kind == "membership" and row["member"] in SPAWN_IDS),
                    "Wildlife must not have group memberships")
            native_rows.append(row)
    require(seen["wildlife_work"] == seen["agent"], "Wildlife workplace read-back incomplete")
    if not bootstrap:
        require(seen["agent"] == SPAWN_IDS and seen["participation"] == SPAWN_IDS and
                seen["type"] == set(ROLE_ENTRIES.values()), "Wildlife bootstrap incomplete")
    # Use the unchanged native validator, including its explicit offline
    # bootstrap phase. Existing rows always retain the full native contract;
    # normal runtime/preflight still requires all 100 authored spawns.
    settlement.validate_snapshot(native_rows, population.settlement, bootstrap=bootstrap)


def bootstrap_sql(population: Population) -> str:
    statements = ["START TRANSACTION;"]
    for actor in population.actors:
        statements.append(f"""INSERT INTO world.ai_spawn_participation_defaults (spawn_id,participation_mode)
 SELECT {actor.spawn_id},0 WHERE NOT EXISTS(SELECT 1 FROM world.ai_spawn_participation_defaults WHERE spawn_id={actor.spawn_id});
INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,
 home_map_id,home_x,home_y,home_z,home_o,world_faction_id)
 SELECT {actor.spawn_id},{actor.agent_type},725,{actor.spawn_id},0,725,
 {actor.x},{actor.y},{actor.z},{actor.orientation},0
 WHERE NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE agent_id={actor.spawn_id} OR spawn_id={actor.spawn_id});""")
    for role, entry in sorted(ROLE_ENTRIES.items()):
        statements.append(f"""INSERT INTO world.ai_agent_type_entry_defaults (creature_entry,agent_type)
 SELECT {entry},{ROLE_TYPES[role]} WHERE NOT EXISTS(SELECT 1 FROM world.ai_agent_type_entry_defaults WHERE creature_entry={entry});""")
    statements.append("COMMIT;")
    return "\n".join(statements) + "\n"


def _spawn_binding(actor: settlement.Actor, respawn: int) -> str:
    return f"""(c.guid={actor.spawn_id} AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND c.zoneId=4988 AND c.areaId=4988 AND c.spawnMask=1 AND c.phaseMask=1
 AND c.equipment_id=-1 AND c.MovementType=0 AND c.wander_distance=0 AND c.spawntimesecs={respawn}
 AND c.ScriptName='' AND c.npcflag=0 AND c.unit_flags=0 AND c.dynamicflags=0
 AND ABS(c.position_x-{actor.x})<=0.001 AND ABS(c.position_y-{actor.y})<=0.001
 AND ABS(c.position_z-{actor.z})<=0.001 AND ABS(c.orientation-{actor.orientation})<=0.001)"""


def activation_sql(population: Population) -> str:
    """Idempotent all-twenty activation after strict, locked offline preflight.

    SQL rechecks immutable wildlife bindings, all 100 native spawn bindings,
    groups and outside ownership. The full native behavior/legacy digest is
    checked by validate_snapshot before and after this transaction.
    """
    bindings = []
    for actor in population.actors:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    native_bindings = " OR ".join(_spawn_binding(actor, population.respawn_seconds)
                                   for actor in population.settlement.actors)
    template_bindings = []
    columns = {"entry": "entry", "ai_name": "AIName", "script_name": "ScriptName",
               "regenerate_health": "RegenHealth", "vehicle": "VehicleId"}
    for template in population.settlement.templates:
        if template["entry"] in ROLE_ENTRIES.values():
            fields = []
            for field in settlement.TEMPLATE_FIELDS:
                value = template[field]
                literal = "'" + value.replace("'", "''") + "'" if isinstance(value, str) else str(value)
                fields.append(f"ct.{columns.get(field, field)}={literal}")
            template_bindings.append("(" + " AND ".join(fields) + ")")
    allowed_ids = ",".join(str(identity) for identity in sorted(settlement.SPAWN_IDS | settlement.LEGACY_IDS))
    legacy_ids = ",".join(str(identity) for identity in sorted(settlement.LEGACY_IDS))
    ids = population.spawn_ids
    return f"""START TRANSACTION;
SET @wildlife_population_ready = (
 SELECT COUNT(*)=20 FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 INNER JOIN world.ai_agent_type_entry_defaults t ON t.creature_entry=c.id
 WHERE a.map_id=725 AND a.home_map_id=725 AND a.control_mode IN (0,1)
 AND a.work_map_id IS NULL AND a.work_x IS NULL AND a.work_y IS NULL
 AND a.work_z IS NULL AND a.work_o IS NULL AND a.world_faction_id=0
 AND p.participation_mode=0 AND t.agent_type=a.agent_type
 AND ({' OR '.join(template_bindings)})
 AND ({' OR '.join(bindings)})
) AND (SELECT COUNT(*) FROM world.creature c WHERE c.map=725 AND ({native_bindings}))=100
 AND NOT EXISTS(SELECT 1 FROM world.creature WHERE map=725 AND guid NOT IN ({allowed_ids}))
 AND NOT EXISTS(SELECT 1 FROM characters.ai_agent_group_members WHERE member_agent_id IN ({ids}))
 AND NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE control_mode=1
   AND NOT (agent_id=spawn_id AND agent_id IN ({ids}) AND map_id=725))
 AND NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE map_id=725
   AND agent_id NOT IN ({ids},{legacy_ids}))
 AND NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE (agent_id IN ({legacy_ids}) OR spawn_id IN ({legacy_ids}))
   AND (agent_id<>spawn_id OR map_id<>725 OR control_mode<>0));
UPDATE characters.ai_agents SET control_mode=1
 WHERE @wildlife_population_ready=1 AND agent_id=spawn_id AND agent_id IN ({ids})
 AND map_id=725 AND control_mode=0;
COMMIT;
"""


def deactivation_sql(population: Population) -> str:
    """Explicit, narrow ownership pause; identities and need state survive."""
    return f"""START TRANSACTION;
UPDATE characters.ai_agents SET control_mode=0
 WHERE agent_id=spawn_id AND agent_id IN ({population.spawn_ids}) AND map_id=725 AND control_mode=1;
COMMIT;
"""
