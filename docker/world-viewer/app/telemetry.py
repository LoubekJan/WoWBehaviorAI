"""Validated, bounded telemetry protocol for the read-only world viewer."""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import AfterValidator, BaseModel, ConfigDict, Field, field_validator, model_validator

MAX_AGENTS = 10_000
# Full Elwynn snapshots with v4 recovery diagnostics exceed the former 8 MiB.
MAX_REQUEST_BYTES = 12 * 1024 * 1024
STALE_AFTER_MS = 5_000
MEMORY_PAGE_SIZE = 25


class ProtocolModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class Point(ProtocolModel):
    x: float
    y: float
    z: float
    map_id: int = Field(ge=0)


class Position(Point):
    source: Literal["live", "spawn", "last_known"]


class Needs(ProtocolModel):
    health_pressure: float = Field(ge=0, le=1)
    hunger: float = Field(ge=0, le=1)
    fatigue: float = Field(ge=0, le=1)
    safety_pressure: float = Field(ge=0, le=1)
    resource_pressure: float = Field(ge=0, le=1)


class Economy(ProtocolModel):
    # Copper is a uint64 in TrinityCore; a decimal string survives JS exactly.
    money: str = Field(pattern=r"^\d{1,20}$")
    food: int = Field(ge=0)
    resource: int = Field(ge=0)


def uint64_decimal(value: str) -> str:
    if int(value) > 2**64 - 1:
        raise ValueError("Counter exceeds uint64")
    return value


Counter64 = Annotated[str, Field(pattern=r"^(0|[1-9][0-9]{0,19})$", strict=True), AfterValidator(uint64_decimal)]
Quantity32 = Annotated[int, Field(ge=0, le=2**32 - 1, strict=True)]


class FoodDepot(ProtocolModel):
    id: int = Field(gt=0, le=2**32 - 1, strict=True)
    name: str = Field(min_length=1, max_length=120, strict=True)
    position: Point
    stock: Quantity32
    capacity: int = Field(gt=0, le=2**32 - 1, strict=True)
    low_threshold: Quantity32
    delivered: Counter64
    consumed: Counter64
    stolen: Counter64 = "0"
    exported: Counter64 = "0"

    @model_validator(mode="after")
    def bounded_stock(self):
        if self.stock > self.capacity or self.low_threshold > self.capacity:
            raise ValueError("Depot stock or low threshold exceeds capacity")
        return self


class FoodAccounting(ProtocolModel):
    initial_food: Counter64
    harvested: Counter64
    in_depots: Counter64
    in_cargo: Counter64
    consumed: Counter64
    lost: Counter64
    conserved: bool = Field(strict=True)

    @model_validator(mode="after")
    def accurate_conservation_flag(self):
        conserved = int(self.initial_food) + int(self.harvested) == sum(
            int(value) for value in (self.in_depots, self.in_cargo, self.consumed, self.lost))
        if self.conserved != conserved:
            raise ValueError("Food conservation flag differs from quantities")
        return self


class LivingCaravan(ProtocolModel):
    id: int = Field(ge=1, le=2, strict=True)
    leader_spawn_id: Counter64
    member_spawn_ids: list[Counter64] = Field(min_length=2, max_length=2)
    source_depot_id: int = Field(gt=0, le=2**32 - 1, strict=True)
    destination_depot_id: Literal[7]
    stage: str = Field(min_length=1, max_length=80, strict=True)
    trip: Counter64
    waypoint: Quantity32
    member_ready: bool = Field(strict=True)
    loaded: Counter64
    completed_trips: Counter64
    source_depot: FoodDepot | None = None
    destination_depot: FoodDepot | None = None

    @model_validator(mode="after")
    def authored_pair_and_warehouses(self):
        leader, guard, villages = ("901040", "901042", (4, 5)) if self.id == 1 else ("901041", "901043", (6, 1))
        if self.leader_spawn_id != leader or self.member_spawn_ids != [leader, guard]:
            raise ValueError("Caravan requires its stable authored carrier and escort")
        if self.source_depot_id not in villages:
            raise ValueError("Caravan source must belong to its two assigned villages")
        for depot, identity in ((self.source_depot, self.source_depot_id),
                                (self.destination_depot, self.destination_depot_id)):
            if depot is not None and (depot.id != identity or depot.position.map_id != 725):
                raise ValueError("Caravan warehouses require matching IDs on map 725")
        return self


class LivingSupply(ProtocolModel):
    enabled: bool = Field(strict=True)
    role: Literal["HUNTER", "PREDATOR", "CONSUMER", "RAIDER", "FARMER", "CARAVAN_CARRIER", "CARAVAN_ESCORT", "CITY_CONSUMER"]
    phase: str = Field(min_length=1, max_length=80, strict=True)
    depot_id: Quantity32
    cargo: Quantity32
    capacity: Quantity32
    carcass_remaining: Quantity32 | None = None
    carcass_spawn_id: Counter64 | None = None
    carcass_life: Counter64 | None = None
    hunting_ground: Point | None = None
    harvested: Counter64
    delivered: Counter64
    consumed: Counter64
    lost: Counter64
    stolen: Counter64 = "0"
    last_result: str = Field(max_length=100, strict=True)
    depot: FoodDepot | None = None
    accounting: FoodAccounting | None = None
    caravan: LivingCaravan | None = None
    field_id: Quantity32 | None = None
    field_workpoint_index: int | None = Field(default=None, ge=0, le=11, strict=True)
    field_workpoint: Point | None = None
    field_remaining: Quantity32 | None = None
    field_capacity: Quantity32 | None = None

    @model_validator(mode="after")
    def consistent_cargo_and_depot(self):
        if self.cargo > self.capacity:
            raise ValueError("Cargo exceeds carrying capacity")
        field_values = (self.field_id, self.field_workpoint_index, self.field_workpoint,
                        self.field_remaining, self.field_capacity)
        if self.role == "FARMER":
            if (any(value is None for value in field_values) or self.field_id not in (4, 5, 6) or
                self.depot_id != self.field_id or self.capacity != 2 or self.depot is None or
                self.depot.capacity != 30 or self.depot.low_threshold != 4 or
                self.field_capacity != 24 or self.field_remaining > self.field_capacity or
                self.hunting_ground is not None or self.carcass_remaining is not None or
                self.carcass_spawn_id is not None or self.carcass_life is not None):
                raise ValueError("Farmers require a bounded crop field, own warehouse and at most two carried portions")
            if self.field_workpoint.map_id != self.depot.position.map_id:
                raise ValueError("Farmer field and warehouse must remain on the same map")
        elif any(value is not None for value in field_values):
            raise ValueError("Only farmers may expose crop field observations")
        if self.role != "RAIDER" and int(self.stolen) != 0:
            raise ValueError("Only raiders can withdraw stolen food")
        caravan_role = self.role in ("CARAVAN_CARRIER", "CARAVAN_ESCORT")
        if caravan_role:
            carrier = self.role == "CARAVAN_CARRIER"
            if (self.caravan is None or self.depot_id != 7 or self.depot is None or
                self.capacity != (6 if carrier else 0) or int(self.harvested) != 0 or
                self.hunting_ground is not None or self.carcass_remaining is not None or
                self.carcass_spawn_id is not None or self.carcass_life is not None or
                (not carrier and any(int(value) != 0 for value in
                                    (self.delivered, self.lost, self.caravan.loaded)))):
                raise ValueError("Caravans require their paired transport capability and city warehouse")
        elif self.caravan is not None:
            raise ValueError("Only caravan participants may expose caravan coordination")
        if self.role == "CITY_CONSUMER" and self.depot_id != 7:
            raise ValueError("City consumers eat only from the city warehouse")
        if self.role == "RAIDER" and (self.capacity != 2 or int(self.harvested) != 0 or
            self.hunting_ground is not None or self.carcass_remaining is not None or
            self.carcass_spawn_id is not None or self.carcass_life is not None or
            self.depot_id == 0 or self.depot is None):
            raise ValueError("Raiders carry at most two stolen portions without hunting capability")
        if self.role in ("CONSUMER", "CITY_CONSUMER") and (self.cargo != 0 or self.capacity != 0 or
            any(int(value) != 0 for value in (self.harvested, self.delivered, self.lost)) or
            self.hunting_ground is not None or self.carcass_remaining is not None or
            self.carcass_spawn_id is not None or self.carcass_life is not None or
            self.depot_id == 0 or self.depot is None):
            raise ValueError("Depot consumers cannot hunt, carry, deliver or expose carcass state")
        if (self.carcass_spawn_id is None) != (self.carcass_life is None):
            raise ValueError("Carcass source and incarnation must be supplied together")
        if self.carcass_spawn_id is not None and (int(self.carcass_spawn_id) == 0 or int(self.carcass_life) == 0):
            raise ValueError("Carcass source and incarnation must be positive")
        if self.depot is not None and self.depot.id != self.depot_id:
            raise ValueError("Supply depot ID differs from its snapshot")
        if self.depot is not None and self.hunting_ground is not None and self.depot.position.map_id != self.hunting_ground.map_id:
            raise ValueError("Depot and hunting ground must be on the same map")
        return self


class RaidDangerMemory(ProtocolModel):
    position: Point
    guard_spawn_id: Counter64
    age_ms: int = Field(ge=0, le=2**64 - 1, strict=True)


class RaidPlanning(ProtocolModel):
    # These are the raider's own recalled observations. The source_depot below
    # remains the spectator's real ledger snapshot, not omniscient NPC knowledge.
    reason: str = Field(min_length=1, max_length=80, strict=True)
    memory_records: int = Field(ge=0, le=128, strict=True)
    route_id: int = Field(ge=0, le=2, strict=True)
    known_stock: Quantity32 | None
    stock_age_ms: int | None = Field(ge=0, le=2**64 - 1, strict=True)
    risk: float = Field(ge=0, le=20, strict=True)
    retry_remaining_ms: int = Field(ge=0, le=2**64 - 1, strict=True)
    danger: RaidDangerMemory | None
    checked_positions: int = Field(default=0, ge=0, le=128, strict=True)
    last_check: RaidDangerMemory | None = None

    @model_validator(mode="after")
    def stock_observation_has_age(self):
        if (self.known_stock is None) != (self.stock_age_ms is None):
            raise ValueError("A raider's stock knowledge requires both observed quantity and age")
        if self.known_stock is not None and self.known_stock > 30:
            raise ValueError("Remembered village stock exceeds the reviewed warehouse capacity")
        return self


class RaidParty(ProtocolModel):
    id: Counter64
    leader_spawn_id: Counter64
    member_spawn_ids: list[Counter64] = Field(min_length=1, max_length=5)
    phase: Literal["ASSEMBLING", "OUTBOUND", "RAIDING", "RETURNING", "COMPLETE"]
    role: Literal["LEADER", "MEMBER"]

    @model_validator(mode="after")
    def stable_roster_has_one_leader(self):
        if (self.id == "0" or "0" in self.member_spawn_ids or
            len(set(self.member_spawn_ids)) != len(self.member_spawn_ids) or
            self.leader_spawn_id != "0" and self.leader_spawn_id not in self.member_spawn_ids):
            raise ValueError("Raid parties need a positive identity and unique roster containing their leader")
        return self


class LivingRaid(ProtocolModel):
    enabled: bool = Field(strict=True)
    role: Literal["RAIDER", "GUARD"]
    phase: str = Field(min_length=1, max_length=80, strict=True)
    outcome: str = Field(max_length=100, strict=True)
    attempt_id: Counter64
    target_spawn_id: Counter64
    noticed: Counter64
    interceptions: Counter64
    thefts: Counter64
    delivered: Counter64
    prevented: Counter64
    source_depot_id: int = Field(gt=0, le=2**32 - 1, strict=True)
    camp_depot_id: int = Field(gt=0, le=2**32 - 1, strict=True)
    source_depot: FoodDepot
    camp_depot: FoodDepot
    planning: RaidPlanning | None = None
    party: RaidParty | None = None

    @model_validator(mode="after")
    def distinct_consistent_depots(self):
        if (self.source_depot_id == self.camp_depot_id or self.source_depot.id != self.source_depot_id or
            self.camp_depot.id != self.camp_depot_id or self.source_depot.position.map_id != self.camp_depot.position.map_id):
            raise ValueError("Raid source and camp must be distinct matching depot snapshots on the same map")
        if self.planning is not None:
            if self.role != "RAIDER":
                raise ValueError("Raid planning memory belongs only to the raider")
            for observation in (self.planning.danger, self.planning.last_check):
                if observation is not None and observation.position.map_id != self.camp_depot.position.map_id:
                    raise ValueError("Remembered danger and checked positions must remain on the raid map")
        if self.party is not None and self.role != "RAIDER":
            raise ValueError("A raid party belongs only to raiders")
        return self


class NavigationDiagnostics(ProtocolModel):
    mesh: bool
    start_tile: bool
    end_tile: bool
    filter: int = Field(ge=0, le=65535)
    start_flags: int = Field(ge=0, le=65535)
    end_flags: int = Field(ge=0, le=65535)
    start_distance: float | None = Field(ge=0)
    end_distance: float | None = Field(ge=0)
    swimming: bool
    rejoin: bool
    failure: str = Field(max_length=80)
    detail: str = Field(default="NONE", max_length=80)
    home_radius: float = Field(default=0, ge=0)
    rejected_x: float | None = None
    rejected_y: float | None = None
    rejected_z: float | None = None
    source_z: float | None = Field(default=None, strict=True)
    support_z: float | None = Field(default=None, strict=True)
    source_x: float | None = Field(default=None, strict=True)
    source_y: float | None = Field(default=None, strict=True)
    projection_x: float | None = Field(default=None, strict=True)
    projection_y: float | None = Field(default=None, strict=True)
    projection_z: float | None = Field(default=None, strict=True)
    projection_ground_z: float | None = Field(default=None, strict=True)
    rejected_ground_z: float | None = Field(default=None, strict=True)
    previous_ground_z: float | None = Field(default=None, strict=True)
    connector_samples: int = Field(default=0, ge=0, le=64, strict=True)
    projection_failure: str = Field(default="NONE", max_length=80)
    projection_probes: int = Field(default=0, ge=0, le=64, strict=True)


class HomePathRejections(ProtocolModel):
    ground: int = Field(default=0, ge=0, le=9, strict=True)
    path: int = Field(default=0, ge=0, le=9, strict=True)
    endpoint: int = Field(default=0, ge=0, le=9, strict=True)
    bounds: int = Field(default=0, ge=0, le=9, strict=True)
    danger: int = Field(default=0, ge=0, le=9, strict=True)
    corridor: int = Field(default=0, ge=0, le=9, strict=True)


class ReturnRecovery(ProtocolModel):
    failures: int = Field(ge=0)
    trail_points: int = Field(ge=0, le=64)
    retry_ms: int = Field(ge=0)
    stalled_ms: int = Field(ge=0)
    strategy: str = Field(max_length=80)
    failure: str = Field(max_length=100)
    candidates: int = Field(ge=0)
    path_type: int = Field(ge=0)
    requested_z: float
    resolved_z: float | None
    navigation: NavigationDiagnostics | None = None
    backtracks: int = Field(default=0, ge=0, le=16)
    rejoins: int = Field(default=0, ge=0, le=8)
    corridor_points: int = Field(default=0, ge=0, le=128)
    home_path_type: int = Field(default=0, ge=0, le=255, strict=True)
    home_path_failure: str = Field(default="NOT_CHECKED", max_length=100)
    home_surface_failure: str = Field(default="NOT_CHECKED", max_length=100, strict=True)
    home_detour_failure: str = Field(default="NOT_CHECKED", max_length=100, strict=True)
    home_detour_nodes: int = Field(default=0, ge=0, le=4294967295, strict=True)
    home_detour_edges: int = Field(default=0, ge=0, le=4294967295, strict=True)
    home_path_surface: bool = Field(default=False, strict=True)
    home_path_rejected: HomePathRejections = Field(default_factory=HomePathRejections)
    continuation_path_type: int = Field(default=0, ge=0, le=255, strict=True)
    continuation_path_failure: str = Field(default="NOT_CHECKED", max_length=100)
    continuation_surface_failure: str = Field(default="NOT_CHECKED", max_length=100, strict=True)
    continuation_detour_failure: str = Field(default="NOT_CHECKED", max_length=100, strict=True)
    continuation_detour_nodes: int = Field(default=0, ge=0, le=4294967295, strict=True)
    continuation_detour_edges: int = Field(default=0, ge=0, le=4294967295, strict=True)
    continuation_path_surface: bool = Field(default=False, strict=True)
    planning_deferred: bool = Field(default=False, strict=True)
    surface_corridor: bool = Field(default=False, strict=True)
    continuation_path_rejected: HomePathRejections = Field(default_factory=HomePathRejections)
    refuge_active: bool = Field(default=False, strict=True)
    refuge_episodes: int = Field(default=0, ge=0, strict=True)
    refuge_moves: int = Field(default=0, ge=0, strict=True)
    refuge_blocked: int = Field(default=0, ge=0, strict=True)
    refuge_remaining_ms: int = Field(default=0, ge=0, le=120000, strict=True)
    refuge_anchor: Point | None = None
    rejected: dict[Literal['invalid', 'height', 'zone', 'los', 'path', 'bounds', 'danger'], int] = Field(max_length=7)

    @model_validator(mode="after")
    def nonnegative_rejections(self):
        if any(n < 0 for n in self.rejected.values()):
            raise ValueError("Negative rejection count")
        return self


class AdviceAdmission(ProtocolModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    ready_observed: int = Field(ge=0, le=4294967295)
    entries: int = Field(ge=0, le=4294967295)
    acquire_attempts: int = Field(ge=0, le=4294967295)
    acquired: int = Field(ge=0, le=4294967295)
    early_gates: int = Field(ge=0, le=4294967295)
    local_empty_options: int = Field(ge=0, le=4294967295)
    last_event: Literal['NONE', 'READY_OBSERVED', 'ENTERED', 'ACQUIRE_ATTEMPT',
                        'ACQUIRED', 'EARLY_GATE', 'NO_VALID_OPTIONS']
    last_reason: str = Field(max_length=80)
    last_event_age_ms: int | None = Field(default=None, ge=0, le=18446744073709551615)
    last_acquired_age_ms: int | None = Field(default=None, ge=0, le=18446744073709551615)


class RecoveryAdvice(ProtocolModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    lifetime_ms: int = Field(ge=0)
    enabled: bool
    pending: bool
    status: str = Field(max_length=80)
    requests: int = Field(ge=0)
    selected: int = Field(ge=0)
    started: int = Field(ge=0)
    arrived: int = Field(ge=0)
    home_success: int = Field(ge=0)
    food_success: int = Field(ge=0)
    rejected: int = Field(ge=0)
    unavailable: int = Field(ge=0)
    reused: int = Field(ge=0)
    empty_search_rounds: int = Field(default=0, ge=0, le=2)
    failed_food_advice: int = Field(default=0, ge=0, le=3)
    known_food_places: int = Field(default=0, ge=0, le=8)
    retry_ms: int = Field(default=0, ge=0)
    search_radius: float = Field(default=80, ge=80, le=128)
    queue_wait_ms: int | None = Field(default=None, ge=0)
    queue_size: int = Field(default=0, ge=0, le=2048)
    queue_dispatchable: bool = False
    queue_kind: Literal['NONE', 'RETURN', 'FOOD'] = 'NONE'
    # Missing in older recordings means unavailable, not zero admissions.
    admission: AdviceAdmission | None = None


class ForageDiagnostics(ProtocolModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    scanned_at_ms: int = Field(ge=0)
    search_at_ms: int = Field(ge=0)
    nearby_prey: int = Field(ge=0)
    attackable_prey: int = Field(ge=0)
    reachable_prey: int = Field(ge=0)
    route_attempts: int = Field(ge=0)
    height_rejected: int = Field(ge=0)
    path_rejected: int = Field(ge=0)
    steps_started: int = Field(ge=0)
    navigation: NavigationDiagnostics


class LivingPlanning(ProtocolModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    deferred: bool = False
    reason: Literal['NONE', 'ADMISSION', 'WORK_BUDGET'] = 'NONE'
    stage: Literal['NONE', 'DECISION', 'HUNT', 'FORAGE', 'RETURN', 'ADVICE', 'SUPPLY_CORRIDOR'] = 'NONE'
    # Wall-time ages include explicit stationary care, unlike query work credit.
    wait_ms: int = Field(default=0, ge=0, le=18446744073709551615)
    query_age_ms: int = Field(default=0, ge=0, le=18446744073709551615)
    no_progress_ms: int = Field(default=0, ge=0, le=18446744073709551615)
    resets: int = Field(default=0, ge=0, le=4294967295)


class LivingRole(ProtocolModel):
    enabled: bool
    extensions_enabled: bool
    role: str = Field(max_length=80)
    status: str = Field(max_length=100)
    phase: str = Field(max_length=80)
    activity: str = Field(max_length=80)
    awareness: str = Field(max_length=80)
    movement_purpose: str = Field(max_length=80)
    caution: float = Field(ge=0, le=1)
    # Incarnation marker; older recordings provide no continuity evidence.
    lifetime_ms: int | None = Field(default=None, ge=0, le=18446744073709551615, strict=True)
    started_at_ms: int = Field(ge=0)
    decision_wait_ms: int = Field(ge=0)
    danger_remaining_ms: int = Field(ge=0)
    alarm_remaining_ms: int = Field(ge=0)
    hunt_status: str = Field(max_length=100)
    hunt_end: str = Field(max_length=100)
    assist_status: str = Field(max_length=100)
    nearby_prey: int = Field(ge=0)
    attackable_prey: int = Field(ge=0)
    nearby_allies: int = Field(ge=0)
    allies_in_combat: int = Field(ge=0)
    companion_spawn_id: int = Field(ge=0)
    last_hunt_target_spawn_id: int = Field(ge=0)
    hunt_target_distance: float | None = Field(default=None, ge=0)
    prey_run_speed: float | None = Field(default=None, ge=0)
    prey_move_speed: float | None = Field(default=None, ge=0)
    sprint_multiplier: float | None = Field(default=None, ge=0)
    sprint_remaining_ms: int | None = Field(default=None, ge=0)
    return_recovery: ReturnRecovery | None = None
    advice: RecoveryAdvice | None = None
    forage: ForageDiagnostics | None = None
    planning: LivingPlanning = Field(default_factory=LivingPlanning)
    move_end: str = Field(default="NONE", max_length=80)
    move_no_progress_ms: int = Field(default=0, ge=0)
    move_remaining: float | None = Field(default=None, ge=0)


class NpcAgent(ProtocolModel):
    """Public live cognition metadata; authored lore stays in worldserver."""
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    name: str = Field(min_length=1, max_length=32)
    status: str = Field(max_length=80)
    last_action_result: str = Field(max_length=128)
    active_action: str = Field(max_length=32)
    request_id: Counter64
    next_think_ms: int = Field(ge=0, le=2**64 - 1)
    dialogue_count: int = Field(ge=0, le=12)
    memory_records: int = Field(default=0, ge=0, le=96)
    memory_recalled: int = Field(default=0, ge=0, le=6)
    last_speech: str = Field(max_length=256)

    @field_validator("name", "status", "last_action_result", "active_action", "last_speech")
    @classmethod
    def plain_bounded_text(cls, value, info):
        for char in value:
            scalar = ord(char)
            if (scalar < 0x20 or 0x7F <= scalar <= 0x9F or char == '|'
                    or 0x200B <= scalar <= 0x200F or 0x2028 <= scalar <= 0x202E
                    or 0x2060 <= scalar <= 0x206F or scalar == 0xFEFF):
                raise ValueError("NPC metadata requires plain text")
        limits = {"name": 80, "status": 80, "last_action_result": 128, "active_action": 32, "last_speech": 768}
        try:
            if len(value.encode("utf-8")) > limits[info.field_name]:
                raise ValueError("NPC metadata exceeds UTF-8 byte limit")
        except UnicodeEncodeError as exc:
            raise ValueError("NPC metadata requires valid UTF-8") from exc
        if info.field_name == "name" and not any(char not in (' ', '\u00a0') for char in value):
            raise ValueError("NPC name must be visible")
        return value


class Movement(ProtocolModel):
    moving: bool
    blocked: bool
    cannot_reach_target: bool
    evading: bool
    run_speed: float = Field(ge=0)
    move_speed: float = Field(ge=0)
    home_distance: float = Field(ge=0)


class Group(ProtocolModel):
    id: int = Field(gt=0)
    kind: str = Field(max_length=80)
    profile: str = Field(max_length=80)
    member_count: int = Field(ge=0)
    resources: float = Field(ge=0)
    territory: Point


class Target(ProtocolModel):
    spawn_id: int = Field(ge=0)
    entry: int = Field(ge=0)
    name: str = Field(max_length=200)
    position: Point


class Agent(ProtocolModel):
    agent_id: int = Field(ge=0)
    spawn_id: int = Field(ge=0)
    native_creature: bool = Field(default=False, strict=True)
    runtime_guid: str | None = Field(default=None, max_length=80, strict=True)
    entry: int | None = Field(default=None, ge=0)
    # entry retains the behavior/source identity; a dedicated character has
    # its own native template/GUID entry in addition to that base identity.
    template_entry: int | None = Field(default=None, ge=1, le=0xFFFFFF, strict=True)
    name: str | None = Field(default=None, max_length=200)
    type: str = Field(max_length=80)
    control_mode: str = Field(max_length=80)
    world_faction: int | str
    reputation_faction_id: int | None = Field(default=None, ge=0)
    faction_template_id: int | None = Field(default=None, ge=0)
    world_state: str = Field(max_length=80)
    simulation_tier: str = Field(max_length=80)
    position: Position
    health: int | None = Field(default=None, ge=0)
    max_health: int | None = Field(default=None, ge=0)
    alive: bool | None = None
    in_combat: bool | None = None
    needs: Needs | None = None
    goal: str | None = Field(default=None, max_length=120)
    goal_utility: float | None = None
    action: str | None = Field(default=None, max_length=120)
    routine_goal: str | None = Field(default=None, max_length=120)
    group_id: int | None = Field(default=None, ge=0)
    economy: Economy | None = None
    home: Point | None = None
    work: Point | None = None
    groups: list[Group] = Field(default_factory=list)
    living_role: LivingRole | None = None
    supply: LivingSupply | None = None
    raid: LivingRaid | None = None
    llm_agent: NpcAgent | None = None
    movement: Movement | None = None
    living_wolf: bool = False
    meal_target_spawn_id: int | None = Field(default=None, ge=0)
    effective_goal: str | None = Field(default=None, max_length=120)
    goal_owner: str = Field(default="", max_length=80)
    action_source_goal: str | None = Field(default=None, max_length=120)
    action_started_at_ms: int | None = Field(default=None, ge=0)
    coordination_goal: str | None = Field(default=None, max_length=120)
    coordination_group_id: int | None = Field(default=None, ge=0)
    coordination_phase: str = Field(default="", max_length=80)
    routine_activity: str = Field(default="", max_length=80)
    destination: Point | None = None
    target: Target | None = None

    @model_validator(mode="after")
    def current_observations_require_live_position(self):
        if self.native_creature:
            if self.control_mode != "OBSERVE_ONLY":
                raise ValueError("Native creatures must remain observation-only")
            if self.needs is not None or self.economy is not None or self.living_role is not None or self.living_wolf or self.supply is not None or self.raid is not None or self.llm_agent is not None:
                raise ValueError("Native creatures do not have AI needs, economy or living-role state")
            if self.position.source != "live" and any(value is not None for value in (
                self.health, self.max_health, self.alive, self.in_combat,
            )):
                raise ValueError("Unloaded native creatures do not have live observations")
        elif self.needs is None:
            raise ValueError("AI agents require needs")
        if self.llm_agent is not None and self.control_mode != "AI_WORLD_CONTROLLED":
            raise ValueError("NPC cognition requires an AI-controlled actor")
        if self.supply is not None:
            if self.control_mode != "AI_WORLD_CONTROLLED":
                raise ValueError("Supply observations require an AI-controlled actor")
            for point in (self.supply.hunting_ground, self.supply.field_workpoint,
                          self.supply.depot.position if self.supply.depot else None):
                if point is not None and point.map_id != self.position.map_id:
                    raise ValueError("Supply observations must remain on the actor's map")
            caravan = self.supply.caravan
            if caravan is not None:
                if self.position.map_id != 725 or str(self.spawn_id) not in caravan.member_spawn_ids:
                    raise ValueError("Caravan observation must belong to its live authored participant")
                if (self.supply.role == "CARAVAN_CARRIER") != (str(self.spawn_id) == caravan.leader_spawn_id):
                    raise ValueError("Caravan carrying capability must agree with the leader's identity")
        if self.raid is not None:
            if self.control_mode != "AI_WORLD_CONTROLLED" or self.supply is None or not self.supply.enabled:
                raise ValueError("Raid observations require a controlled actor with current supply observations")
            if self.supply.role != ("RAIDER" if self.raid.role == "RAIDER" else "CONSUMER"):
                raise ValueError("Raid role must match the actor's supply capability")
            for depot in (self.raid.source_depot, self.raid.camp_depot):
                if depot.position.map_id != self.position.map_id:
                    raise ValueError("Raid depots must remain on the actor's map")
            party = self.raid.party
            if party is not None:
                if str(self.spawn_id) not in party.member_spawn_ids:
                    raise ValueError("A raider must belong to its party's stable roster")
                if (party.role == "LEADER") != (party.leader_spawn_id == str(self.spawn_id)):
                    raise ValueError("Raid party leadership must agree with the actor's identity")
        if self.position.source != "live" and any((
            self.living_role, self.movement, self.target, self.destination, self.living_wolf,
            self.supply, self.raid, self.llm_agent,
            self.meal_target_spawn_id is not None,
            self.faction_template_id is not None,
            self.runtime_guid is not None,
        )):
            raise ValueError("Live observations require a live position")
        return self


class MemoryEntity(ProtocolModel):
    kind: Literal["PLAYER", "CREATURE", "UNKNOWN"]
    name: str = Field(max_length=200)
    agent_id: str = Field(pattern=r"^[0-9]{1,20}$")
    spawn_id: str = Field(pattern=r"^[0-9]{1,20}$")
    entry: int = Field(ge=0)


class LongTermMemory(ProtocolModel):
    persistent_id: str = Field(pattern=r"^[0-9]{1,20}$")
    type: str = Field(max_length=80)
    importance: float = Field(ge=0, le=1)
    event_type: str | None = Field(max_length=100)
    source_event_id: str = Field(pattern=r"^[0-9]{1,20}$")
    correlation_id: str = Field(pattern=r"^[0-9]{1,20}$")
    source_occurred_at_ms: int = Field(ge=0)
    first_observed_at_ms: int = Field(ge=0)
    last_observed_at_ms: int = Field(ge=0)
    observation_count: int = Field(ge=0)
    channel: str = Field(max_length=80)
    location: Point
    actor: MemoryEntity
    target: MemoryEntity


class MemoryPage(ProtocolModel):
    request_id: str = Field(pattern=r"^[a-f0-9]{32}$")
    agent_id: int = Field(gt=0)
    offset: int = Field(ge=0, le=2**32 - 1)
    requested_anchor: int = Field(ge=0, le=2**32 - 1)
    anchor: int = Field(ge=0)
    total: int = Field(ge=0)
    records: list[LongTermMemory] = Field(max_length=MEMORY_PAGE_SIZE)

    @model_validator(mode="after")
    def valid_page_bounds(self):
        if self.anchor > self.total or len(self.records) != min(MEMORY_PAGE_SIZE, max(0, self.anchor - self.offset)):
            raise ValueError("Inconsistent memory page bounds")
        return self


class TelemetryBatch(ProtocolModel):
    version: Literal[1, 2, 3, 4]
    captured_at_ms: int = Field(ge=0)
    agents: list[Agent] = Field(max_length=MAX_AGENTS)
    memory_page: MemoryPage | None = None

    @model_validator(mode="after")
    def native_creatures_do_not_supply_ai_memory(self):
        if self.memory_page is not None and any(
            agent.native_creature and agent.agent_id == self.memory_page.agent_id for agent in self.agents
        ):
            raise ValueError("Native creatures cannot supply AI memory pages")
        return self

    @model_validator(mode="after")
    def supply_copies_share_one_ledger_snapshot(self):
        depots = {}
        fields = {}
        accounting = None
        for agent in self.agents:
            copies = []
            if agent.supply is not None and agent.supply.field_id is not None:
                current_field = (agent.supply.field_remaining, agent.supply.field_capacity)
                if agent.supply.field_id in fields and fields[agent.supply.field_id] != current_field:
                    raise ValueError("Conflicting copies of the same crop field")
                fields[agent.supply.field_id] = current_field
            if agent.supply is not None and agent.supply.depot is not None:
                copies.append(agent.supply.depot)
            if agent.supply is not None and agent.supply.caravan is not None:
                copies.extend(depot for depot in (agent.supply.caravan.source_depot,
                                                   agent.supply.caravan.destination_depot) if depot is not None)
            if agent.raid is not None:
                copies.extend((agent.raid.source_depot, agent.raid.camp_depot))
            for copy in copies:
                depot = copy.model_dump()
                if copy.id in depots and depots[copy.id] != depot:
                    raise ValueError("Conflicting copies of the same food depot")
                depots[copy.id] = depot
            if agent.supply is not None and agent.supply.accounting is not None:
                current = agent.supply.accounting.model_dump()
                if accounting is not None and accounting != current:
                    raise ValueError("Conflicting copies of the food accounting snapshot")
                accounting = current
        return self

    @model_validator(mode="after")
    def raid_party_copies_share_one_objective(self):
        parties = {}
        for agent in self.agents:
            raid = agent.raid
            if raid is None or raid.party is None:
                continue
            party = raid.party
            shared = (party.leader_spawn_id, tuple(party.member_spawn_ids), party.phase,
                      raid.attempt_id, raid.source_depot_id,
                      raid.planning.route_id if raid.planning is not None else None)
            if party.id in parties and parties[party.id] != shared:
                raise ValueError("Raid party copies disagree about their roster or common objective")
            parties[party.id] = shared
        return self

    @model_validator(mode="after")
    def caravan_copies_share_one_objective(self):
        caravans = {}
        for agent in self.agents:
            caravan = agent.supply.caravan if agent.supply is not None else None
            if caravan is None:
                continue
            shared = (caravan.leader_spawn_id, tuple(caravan.member_spawn_ids), caravan.stage,
                      caravan.trip, caravan.waypoint, caravan.source_depot_id,
                      caravan.destination_depot_id, caravan.completed_trips)
            if caravan.id in caravans and caravans[caravan.id] != shared:
                raise ValueError("Caravan copies disagree about their roster or common route")
            caravans[caravan.id] = shared
        return self
