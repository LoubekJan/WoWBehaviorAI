"""AI bridge - health endpoint + versioned decision protocol stub (Etapa 1
section 1.11 / Etapa 2 sections 2.9/2.9A/2.9B).

No real inference yet - this exists so worldserver's async transport
(resolve/connect/write/read, timeouts, stale-response rejection) and, since
2.9B, its structured DecisionResponse handling can be exercised end-to-end
before any model is involved. Milestone 2.9A widened the request body from
a flat snapshot into a versioned DecisionRequest carrying the agent's full
AgentContext (Needs, ActiveGoal, Top-N RelevantMemories, explicit
AvailableActions).

CURRENT_PROTOCOL_VERSION is an actual compatibility gate (2.9A P2 fix), not
just an echo: a request naming any other protocol_version is rejected
before its body is trusted, and the response always reports the server's
own CURRENT_PROTOCOL_VERSION rather than parroting back whatever the
request claimed.

Milestone 2.9B replaced the response's opaque action string with a
structured decision.type - an incompatible wire-shape change a V1 peer's
parser can't just ignore, hence CURRENT_PROTOCOL_VERSION bumping to 2
alongside it (2.9B P2 fix; no backward compatibility with V1 is needed, a
mismatched version is simply rejected, see the check below) - and gave
this stub its first (still fully deterministic, still no model) policy:
propose FLEE only when the agent's own ActiveGoal already is FLEE_DANGER
and FLEE is one of the AvailableActions this same request offered, else
propose NONE.
GET_FOOD/hunger deliberately always gets NONE back too - worldserver
already owns a verified deterministic MOVE_TO -> EAT lifecycle for it (see
AIWorldMgr::TryEat()), and 2.9B must not introduce a second, competing
owner of that same action. worldserver's own AIWorldMgr::Update() only
logs whatever decision.type comes back - see its 2.9B comment for why this
stub proposing FLEE never actually moves anything by itself.
"""
from typing import List, Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

app = FastAPI(title="ai-server", version="0.1.0")

CURRENT_PROTOCOL_VERSION = 2


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


class DecisionIntent(BaseModel):
    type: str


class DecisionResponse(BaseModel):
    protocol_version: int
    request_id: int
    agent_id: int
    snapshot_sequence: int
    decision: DecisionIntent


def _propose_intent(context: AgentContext) -> str:
    """Deterministic 2.9B policy - see module docstring. FLEE only when
    both the agent's own current goal and this same request's own
    available_actions already say FLEE is the situation; GET_FOOD/hunger
    stays NONE on purpose, worldserver's own MOVE_TO -> EAT lifecycle
    already owns that action.
    """

    goal = context.active_goal
    if goal is not None and goal.type == "FLEE_DANGER" and "FLEE" in context.available_actions:
        return "FLEE"

    return "NONE"


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
        decision=DecisionIntent(type=_propose_intent(request.agent_context)),
    )
