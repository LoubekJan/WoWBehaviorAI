"""Reviewed predator/prey population on the isolated realm lab.

The map proof is shared with single-return. Population writes target only the
lab world/characters tables and preserve every existing home and identity.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import re

try:
    from . import single_return as base
except ImportError:
    import single_return as base

METADATA = base.METADATA
ProfileError = base.ProfileError
require = base.require
ROLE_TYPES = {"predator": 6, "prey": 7}
ROLE_ENTRIES = {"predator": 1186, "prey": 883}
EXPECTED_IDS = frozenset(range(900725, 900731))
# Profiles are reviewed independently; growing the population never changes the
# six-actor regression profile or makes an arbitrary metadata count acceptable.
PROFILES = {
    "hunt-cycle": ("hunt-population.json", 6, 2, 4),
    "hunt-100": ("hunt-population-100.json", 100, 20, 80),
    "hunt-terrain-100": ("hunt-population-terrain-100.json", 100, 20, 80),
}
# Non-attackable, immune to NPC attacks, and non-selectable templates cannot
# demonstrate a physical hunt/kill/feed cycle.
FORBIDDEN_TEMPLATE_FLAGS = 0x2 | 0x200 | 0x02000000


def integer(value, low: int, high: int, name: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool) and low <= value <= high,
            f"Invalid population {name}")
    return value


@dataclass(frozen=True)
class Actor:
    spawn_id: int
    entry: int
    role: str
    label: str
    x: float
    y: float
    z: float
    orientation: float

    @property
    def agent_type(self) -> int:
        return ROLE_TYPES[self.role]


# This baseline is deliberate code, rather than another mutable JSON input.
# A new large-population manifest cannot redefine the already deployed actors.
ORIGINAL_ACTORS = (
    Actor(900725, 1186, "predator", "aiworld_lab_single_return", 266.667, 800, 0, 0),
    Actor(900726, 1186, "predator", "aiworld_lab_hunt_900726", 326.667, 800, 0, 0),
    Actor(900727, 883, "prey", "aiworld_lab_hunt_900727", 291.667, 800, 0, 0),
    Actor(900728, 883, "prey", "aiworld_lab_hunt_900728", 256.667, 826, 0, 0),
    Actor(900729, 883, "prey", "aiworld_lab_hunt_900729", 351.667, 800, 0, 0),
    Actor(900730, 883, "prey", "aiworld_lab_hunt_900730", 336.667, 774, 0, 0),
)


@dataclass(frozen=True)
class Population:
    scope: base.Scope
    actors: tuple[Actor, ...]
    respawn_seconds: int
    profile: str = "hunt-cycle"

    @classmethod
    def load(cls, metadata: Path = METADATA, *, profile: str = "hunt-cycle") -> Population:
        require(isinstance(profile, str) and profile in PROFILES, "Unknown reviewed population profile")
        if profile == "hunt-terrain-100":
            try:
                from . import terrain_profile
            except ImportError:
                import terrain_profile
            return terrain_profile.load_population(terrain_profile.METADATA if metadata == METADATA else metadata)
        filename, actor_count, predator_count, prey_count = PROFILES[profile]
        scope = base.Scope.load(metadata)
        document = base.load_json(metadata / filename)
        points = base.load_json(metadata / "test-points.json")
        require(document.get("schema_version") == 1 and document.get("profile") == profile and
                (document.get("map_id"), document.get("area_id")) == (scope.map_id, scope.area_id) and
                document.get("source_revision") == points.get("source_revision"),
                "Population profile/map/source proof mismatch")
        respawn = integer(document.get("respawn_seconds"), 30, 3600, "respawn interval")
        rows = document.get("agents")
        require(isinstance(rows, list) and len(rows) == actor_count,
                f"Reviewed {profile} population requires {actor_count} actors")
        actors = []
        for row in rows:
            require(isinstance(row, dict), "Invalid population actor")
            try:
                spawn = integer(row["spawn_id"], 1, 0xFFFFFFFF, "spawn ID")
                role = row["role"]
                require(isinstance(role, str) and role in ROLE_TYPES and row["entry"] == ROLE_ENTRIES[role],
                        "Population role/entry mismatch")
                label = row["label"]
                expected_label = base.SPAWN_LABEL if spawn == base.SPAWN_ID else f"aiworld_lab_hunt_{spawn}"
                require(isinstance(label, str) and re.fullmatch(r"[a-z0-9_]{1,64}", label) and label == expected_label,
                        "Population spawn label mismatch")
                home = row["home"]
                x, y, z, orientation = (base.finite(home[key]) for key in ("x", "y", "z", "o"))
                require(scope.min_x <= x <= scope.max_x and scope.min_y <= y <= scope.max_y and
                        z == scope.home_z and 0 <= orientation < 2 * math.pi,
                        "Population home escapes reviewed flat scope")
                if spawn == base.SPAWN_ID:
                    require(role == "predator" and base.near(x, scope.home_x) and
                            base.near(y, scope.home_y) and base.near(z, scope.home_z) and orientation == 0,
                            "Original single-return bear home/identity must be preserved")
                actors.append(Actor(spawn, row["entry"], role, label, x, y, z, orientation))
            except (KeyError, TypeError) as exc:
                raise ProfileError("Incomplete population actor metadata") from exc
        require({actor.spawn_id for actor in actors} == frozenset(range(900725, 900725 + actor_count)) and
                sum(actor.role == "predator" for actor in actors) == predator_count and
                sum(actor.role == "prey" for actor in actors) == prey_count,
                f"Population must contain the reviewed {actor_count} IDs, {predator_count} predators and {prey_count} prey")
        if profile == "hunt-100":
            originals = {actor.spawn_id: actor for actor in actors if actor.spawn_id in EXPECTED_IDS}
            require(originals == {actor.spawn_id: actor for actor in ORIGINAL_ACTORS},
                    "Expanded population must preserve all six original identities, roles and homes")
        require(all(math.hypot(a.x - b.x, a.y - b.y) >= 5
                    for index, a in enumerate(actors) for b in actors[index + 1:]),
                "Population homes must be at least five yards apart")
        if profile == "hunt-100":
            # Wildlife initially scans within 25 yards; a dense profile must
            # supply nearby prey without enlarging the verified map or scan.
            require(all(sum(b.role == "prey" and math.hypot(a.x - b.x, a.y - b.y) <= 25
                            for b in actors) >= 2 for a in actors if a.role == "predator"),
                    "Expanded predators require two prey homes within the native 25-yard scan")
        return cls(scope, tuple(sorted(actors, key=lambda actor: actor.spawn_id)), respawn, profile)

    @property
    def spawn_ids(self) -> str:
        return ",".join(str(actor.spawn_id) for actor in self.actors)

    @property
    def entries(self) -> str:
        return ",".join(str(value) for value in sorted({actor.entry for actor in self.actors}))

    def config(self) -> dict[str, str]:
        result = self.scope.config()
        result["AIWorld.ScopeSpawnIds"] = f'"{self.spawn_ids}"'
        return result

    def observer_environment(self) -> dict[str, str]:
        return self.scope.observer_environment()


def verify_data(data: Path, metadata: Path = METADATA, *, profile: str = "hunt-cycle") -> Population:
    if profile == "hunt-terrain-100":
        try:
            from . import terrain_profile
        except ImportError:
            import terrain_profile
        return terrain_profile.verify_data(data, terrain_profile.METADATA if metadata == METADATA else metadata)
    scope = base.verify_data(data, metadata)
    population = Population.load(metadata, profile=profile)
    require(population.scope == scope, "Population geometry differs from verified map")
    return population


def snapshot_sql(population: Population) -> str:
    ids, entries, scope = population.spawn_ids, population.entries, population.scope
    return f"""
SELECT JSON_OBJECT('kind','template','entry',entry,'ai_name',AIName,'script_name',ScriptName,
 'regenerate_health',RegenHealth,'vehicle',VehicleId,'npcflag',npcflag,'unit_flags',unit_flags,'faction',faction)
 FROM world.creature_template WHERE entry IN ({entries});
SELECT JSON_OBJECT('kind','spawn','guid',guid,'entry',id,'map',map,'zone',zoneId,'area',areaId,
 'spawn_mask',spawnMask,'phase_mask',phaseMask,'x',position_x,'y',position_y,'z',position_z,
 'orientation',orientation,'movement',MovementType,'wander',wander_distance,'label',StringId,'script_name',ScriptName,
 'respawn',spawntimesecs,'npcflag',npcflag,'unit_flags',unit_flags,'dynamicflags',dynamicflags)
 FROM world.creature WHERE guid IN ({ids}) OR map={scope.map_id}
 OR StringId='{base.SPAWN_LABEL}' OR StringId LIKE 'aiworld_lab_hunt_%';
SELECT JSON_OBJECT('kind','agent','id',agent_id,'spawn',spawn_id,'map',map_id,'type',agent_type,'control',control_mode,
 'home_map',home_map_id,'home_x',home_x,'home_y',home_y,'home_z',home_z,'home_o',home_o,
 'work_map',work_map_id,'faction',world_faction_id) FROM characters.ai_agents
 WHERE agent_id IN ({ids}) OR spawn_id IN ({ids}) OR map_id={scope.map_id} OR control_mode=1;
SELECT JSON_OBJECT('kind','participation','spawn',spawn_id,'mode',participation_mode)
 FROM world.ai_spawn_participation_defaults WHERE spawn_id IN ({ids});
SELECT JSON_OBJECT('kind','type','entry',creature_entry,'type',agent_type)
 FROM world.ai_agent_type_entry_defaults WHERE creature_entry IN ({entries});
SELECT JSON_OBJECT('kind','membership','group',group_id,'member',member_agent_id)
 FROM characters.ai_agent_group_members WHERE member_agent_id IN ({ids});
"""


parse_snapshot = base.parse_snapshot


def validate_snapshot(rows: list[dict], population: Population, *, control: int | None,
                      bootstrap: bool = False) -> None:
    fields = {
        "template": "entry ai_name script_name regenerate_health vehicle npcflag unit_flags faction",
        "spawn": "guid entry map zone area spawn_mask phase_mask x y z orientation movement wander label script_name respawn npcflag unit_flags dynamicflags",
        "agent": "id spawn map type control home_map home_x home_y home_z home_o work_map faction",
        "participation": "spawn mode", "type": "entry type", "membership": "group member",
    }
    require(control in (None, 0, 1), "Invalid requested population control phase")
    require(all(isinstance(row, dict) and row.get("kind") in fields and
                set(fields[row["kind"]].split()).issubset(row) for row in rows),
            "Incomplete population SQL read-back")
    grouped = {kind: [row for row in rows if row["kind"] == kind] for kind in fields}
    scope = population.scope
    actors = {actor.spawn_id: actor for actor in population.actors}
    templates = {actor.entry: actor.agent_type for actor in population.actors}
    require(len(grouped["template"]) == len(templates) and
            {row["entry"] for row in grouped["template"]} == set(templates),
            "Lab predator/prey templates missing or duplicated")
    for row in grouped["template"]:
        require(not row["ai_name"] and not row["script_name"] and row["regenerate_health"] == 1 and
                row["vehicle"] == 0 and row["npcflag"] == 0 and isinstance(row["unit_flags"], int) and
                row["unit_flags"] & FORBIDDEN_TEMPLATE_FLAGS == 0 and
                isinstance(row["faction"], int) and row["faction"] > 0,
                "Predator/prey template must be native, attackable, healthy wildlife")
    require(not grouped["membership"], "Hunt population must not have AI group memberships")
    for kind, identity in (("spawn", "guid"), ("agent", "id"), ("participation", "spawn")):
        seen = set()
        for row in grouped[kind]:
            key = row[identity]
            require(key in actors and key not in seen, f"Population {kind} ID/map collision or outside controlled agent")
            seen.add(key)
            actor = actors[key]
            if kind == "spawn":
                require((row["entry"], row["map"], row["zone"], row["area"], row["label"]) ==
                        (actor.entry, scope.map_id, scope.area_id, scope.area_id, actor.label),
                        "Population spawn identity/label/map collision")
                require((row["spawn_mask"], row["phase_mask"], row["movement"], row["wander"], row["respawn"],
                         row["npcflag"], row["unit_flags"], row["dynamicflags"]) ==
                        (1, 1, 0, 0, population.respawn_seconds, 0, 0, 0) and not row["script_name"] and
                        all(base.near(row[field], value) for field, value in
                            (("x", actor.x), ("y", actor.y), ("z", actor.z), ("orientation", actor.orientation))),
                        "Population spawn home, respawn or movement changed")
            elif kind == "agent":
                require((row["spawn"], row["map"], row["type"], row["home_map"], row["work_map"], row["faction"]) ==
                        (actor.spawn_id, scope.map_id, actor.agent_type, scope.map_id, None, 0),
                        "Population agent identity/role collision")
                require(all(base.near(row[field], value) for field, value in
                            (("home_x", actor.x), ("home_y", actor.y), ("home_z", actor.z), ("home_o", actor.orientation))),
                        "Population agent immutable home changed")
                require(row["control"] in (0, 1) and (control is None or row["control"] == control),
                        "Population control mode differs from activation phase")
            else:
                require(row["mode"] == 0, "Population participation must be FullAgent")
        if not bootstrap:
            require(seen == set(actors), f"Population {kind} bootstrap incomplete")
    seen_entries = set()
    for row in grouped["type"]:
        require(row["entry"] in templates and row["entry"] not in seen_entries and
                row["type"] == templates[row["entry"]], "Population entry classification mismatch")
        seen_entries.add(row["entry"])
    if not bootstrap:
        require(seen_entries == set(templates), "Population entry classification bootstrap incomplete")


def bootstrap_sql(population: Population) -> str:
    scope = population.scope
    statements = ["START TRANSACTION;"]
    for actor in population.actors:
        statements.append(f"""INSERT INTO world.creature (guid,id,map,zoneId,areaId,spawnMask,phaseMask,
 position_x,position_y,position_z,orientation,spawntimesecs,wander_distance,MovementType,curhealth,curmana,StringId,VerifiedBuild)
 SELECT {actor.spawn_id},{actor.entry},{scope.map_id},{scope.area_id},{scope.area_id},1,1,
 {actor.x},{actor.y},{actor.z},{actor.orientation},{population.respawn_seconds},0,0,0,0,'{actor.label}',12340
 WHERE NOT EXISTS(SELECT 1 FROM world.creature WHERE guid={actor.spawn_id});
INSERT INTO world.ai_spawn_participation_defaults (spawn_id,participation_mode)
 SELECT {actor.spawn_id},0 WHERE NOT EXISTS(SELECT 1 FROM world.ai_spawn_participation_defaults WHERE spawn_id={actor.spawn_id});
INSERT INTO characters.ai_agents (agent_id,agent_type,map_id,spawn_id,control_mode,home_map_id,home_x,home_y,home_z,home_o,world_faction_id)
 SELECT {actor.spawn_id},{actor.agent_type},{scope.map_id},{actor.spawn_id},0,{scope.map_id},{actor.x},{actor.y},{actor.z},{actor.orientation},0
 WHERE NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE agent_id={actor.spawn_id} OR spawn_id={actor.spawn_id});""")
    for entry, agent_type in sorted({actor.entry: actor.agent_type for actor in population.actors}.items()):
        statements.append(f"""INSERT INTO world.ai_agent_type_entry_defaults (creature_entry,agent_type)
 SELECT {entry},{agent_type} WHERE NOT EXISTS(SELECT 1 FROM world.ai_agent_type_entry_defaults WHERE creature_entry={entry});""")
    statements.append(f"""UPDATE characters.ai_agents SET control_mode=0
 WHERE agent_id=spawn_id AND agent_id IN ({population.spawn_ids}) AND map_id={scope.map_id};
COMMIT;""")
    return "\n".join(statements) + "\n"


def activation_sql(population: Population) -> str:
    """The reviewed actors become controlled together only with valid bindings.

    Bootstrap and activation are offline operations: the world is stopped and
    no other lab deployment or writer may run between preflight and read-back.
    """
    scope, ids = population.scope, population.spawn_ids
    bindings = []
    for actor in population.actors:
        bindings.append(f"""(a.agent_id={actor.spawn_id} AND a.spawn_id={actor.spawn_id}
 AND a.agent_type={actor.agent_type} AND c.id={actor.entry} AND c.StringId='{actor.label}'
 AND ABS(c.position_x-{actor.x})<=0.001 AND ABS(c.position_y-{actor.y})<=0.001
 AND ABS(c.position_z-{actor.z})<=0.001 AND ABS(c.orientation-{actor.orientation})<=0.001
 AND ABS(a.home_x-{actor.x})<=0.001 AND ABS(a.home_y-{actor.y})<=0.001
 AND ABS(a.home_z-{actor.z})<=0.001 AND ABS(a.home_o-{actor.orientation})<=0.001)""")
    return f"""START TRANSACTION;
SET @lab_population_ready = (
 SELECT COUNT(*)={len(population.actors)} FROM characters.ai_agents a
 INNER JOIN world.creature c ON c.guid=a.spawn_id AND c.map=a.map_id
 INNER JOIN world.creature_template ct ON ct.entry=c.id
 INNER JOIN world.ai_spawn_participation_defaults p ON p.spawn_id=a.spawn_id
 INNER JOIN world.ai_agent_type_entry_defaults t ON t.creature_entry=c.id
 LEFT JOIN characters.ai_agent_group_members g ON g.member_agent_id=a.agent_id
 WHERE a.map_id={scope.map_id} AND a.control_mode=0 AND a.home_map_id={scope.map_id}
 AND a.work_map_id IS NULL AND a.world_faction_id=0 AND p.participation_mode=0 AND t.agent_type=a.agent_type
 AND g.member_agent_id IS NULL AND c.zoneId={scope.area_id} AND c.areaId={scope.area_id}
 AND c.spawnMask=1 AND c.phaseMask=1 AND c.MovementType=0 AND c.wander_distance=0
 AND c.spawntimesecs={population.respawn_seconds} AND c.ScriptName=''
 AND c.npcflag=0 AND c.unit_flags=0 AND c.dynamicflags=0
 AND ct.AIName='' AND ct.ScriptName='' AND ct.RegenHealth=1 AND ct.VehicleId=0 AND ct.npcflag=0
 AND (ct.unit_flags & {FORBIDDEN_TEMPLATE_FLAGS})=0 AND ct.faction>0
 AND ({' OR '.join(bindings)})
) AND (SELECT COUNT(*) FROM world.creature WHERE map={scope.map_id})={len(population.actors)}
 AND (SELECT COUNT(*) FROM characters.ai_agents WHERE map_id={scope.map_id})={len(population.actors)}
 AND NOT EXISTS(SELECT 1 FROM characters.ai_agents WHERE control_mode=1)
 AND NOT EXISTS(SELECT 1 FROM world.creature
 WHERE (StringId='{base.SPAWN_LABEL}' OR StringId LIKE 'aiworld_lab_hunt_%') AND guid NOT IN ({ids}));
UPDATE characters.ai_agents SET control_mode=1
 WHERE @lab_population_ready=1 AND agent_id=spawn_id AND agent_id IN ({ids}) AND map_id={scope.map_id} AND control_mode=0;
COMMIT;
"""
