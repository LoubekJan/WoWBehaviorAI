"""AI bridge - health endpoint + versioned decision protocol stub (Etapa 1
section 1.11 / Etapa 2 sections 2.9/2.9A).

/decision always returns action="NONE": no real inference yet. This exists
so worldserver's async transport (resolve/connect/write/read, timeouts,
stale-response rejection) can be exercised end-to-end before any model is
involved. Milestone 2.9A widened the request body from a flat snapshot into
a versioned DecisionRequest carrying the agent's full AgentContext (Needs,
ActiveGoal, Top-N RelevantMemories, explicit AvailableActions) - this stub
still ignores all of it and answers deterministically.
"""
from typing import List, Optional

from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI(title="ai-server", version="0.1.0")


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


class Position(BaseModel):
    x: float
    y: float
    z: float
    orientation: float


class NeedsState(BaseModel):
    health_pressure: float
    hunger: float
    fatigue: float
    safety_pressure: float
    resource_pressure: float


class ActiveGoal(BaseModel):
    type: str
    priority: str
    source: str
    utility: float
    started_at_ms: int
    timeout_ms: int


class RetrievedMemory(BaseModel):
    tier: str
    memory_id: int
    type: str
    importance: float
    relevance: float
    source_event_id: int
    source_occurred_at_ms: int
    source_event_type: Optional[str] = None
    first_observed_at_ms: int
    last_observed_at_ms: int


class AgentContext(BaseModel):
    agent_id: int
    snapshot_sequence: int
    spawn_id: int
    entry: int
    map_id: int
    position: Position
    health: int
    max_health: int
    alive: bool
    in_combat: bool
    needs: NeedsState
    active_goal: Optional[ActiveGoal] = None
    relevant_memories: List[RetrievedMemory] = []
    available_actions: List[str] = []


class DecisionRequest(BaseModel):
    protocol_version: int
    request_id: int
    agent_context: AgentContext


class DecisionResponse(BaseModel):
    protocol_version: int
    request_id: int
    agent_id: int
    snapshot_sequence: int
    action: str


@app.post("/decision", response_model=DecisionResponse)
def decision(request: DecisionRequest) -> DecisionResponse:
    return DecisionResponse(
        protocol_version=request.protocol_version,
        request_id=request.request_id,
        agent_id=request.agent_context.agent_id,
        snapshot_sequence=request.agent_context.snapshot_sequence,
        action="NONE",
    )
