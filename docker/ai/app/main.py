"""AI bridge - health endpoint + versioned decision protocol stub (Etapa 1
section 1.11 / Etapa 2 sections 2.9/2.9A).

/decision always returns action="NONE": no real inference yet. This exists
so worldserver's async transport (resolve/connect/write/read, timeouts,
stale-response rejection) can be exercised end-to-end before any model is
involved. Milestone 2.9A widened the request body from a flat snapshot into
a versioned DecisionRequest carrying the agent's full AgentContext (Needs,
ActiveGoal, Top-N RelevantMemories, explicit AvailableActions) - this stub
still ignores all of it and answers deterministically.

CURRENT_PROTOCOL_VERSION is an actual compatibility gate (2.9A P2 fix), not
just an echo: a request naming any other protocol_version is rejected
before its body is trusted, and the response always reports the server's
own CURRENT_PROTOCOL_VERSION rather than parroting back whatever the
request claimed.
"""
from typing import List, Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

app = FastAPI(title="ai-server", version="0.1.0")

CURRENT_PROTOCOL_VERSION = 1


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


class DecisionLocation(BaseModel):
    map_id: int
    x: float
    y: float
    z: float


class DecisionEntity(BaseModel):
    """Wire-safe stand-in for a memory's Actor/Target - entry (public game
    data) and agent_id (0 if not itself a tracked AIWorld agent) only, same
    boundary worldserver's DecisionEntity enforces: never a raw
    ObjectGuid/SpawnId, those are internal engine/DB identity.
    """

    entry: int
    agent_id: int


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
    location: DecisionLocation
    actor: DecisionEntity
    target: DecisionEntity


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
    if request.protocol_version != CURRENT_PROTOCOL_VERSION:
        raise HTTPException(
            status_code=400,
            detail=f"unsupported protocol_version {request.protocol_version}, expected {CURRENT_PROTOCOL_VERSION}",
        )

    return DecisionResponse(
        protocol_version=CURRENT_PROTOCOL_VERSION,
        request_id=request.request_id,
        agent_id=request.agent_context.agent_id,
        snapshot_sequence=request.agent_context.snapshot_sequence,
        action="NONE",
    )
