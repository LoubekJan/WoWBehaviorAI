"""Validated, bounded telemetry protocol for the read-only world viewer."""
from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

MAX_AGENTS = 10_000
MAX_REQUEST_BYTES = 8 * 1024 * 1024
STALE_AFTER_MS = 5_000


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


class TelemetryBatch(ProtocolModel):
    version: Literal[1, 2]
    captured_at_ms: int = Field(ge=0)
    agents: list[Agent] = Field(max_length=MAX_AGENTS)
