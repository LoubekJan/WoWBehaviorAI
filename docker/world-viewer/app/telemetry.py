"""Validated, bounded telemetry protocol for the read-only world viewer."""
from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

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
    rejected: dict[Literal['invalid', 'height', 'zone', 'los', 'path', 'bounds', 'danger'], int] = Field(max_length=7)

    @model_validator(mode="after")
    def nonnegative_rejections(self):
        if any(n < 0 for n in self.rejected.values()):
            raise ValueError("Negative rejection count")
        return self


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
    entry: int | None = Field(default=None, ge=0)
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
    needs: Needs
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
        if self.position.source != "live" and any((
            self.living_role, self.movement, self.target, self.destination, self.living_wolf,
            self.meal_target_spawn_id is not None,
            self.faction_template_id is not None,
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
