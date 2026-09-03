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
import json
from typing import List, Optional

from fastapi import Depends, FastAPI, HTTPException, Request
from pydantic import BaseModel, ValidationError

from .dynamic_task import (
    CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION,
    DynamicTaskRequest,
    DynamicTaskResponse,
    QuestProposalPolicyError,
    validate_draft_against_context,
)
from .model_provider import (
    ModelProviderBadStatus,
    ModelProviderConfig,
    ModelProviderMalformedContent,
    ModelProviderOversizedResponse,
    ModelProviderTimeout,
    ModelProviderUnavailable,
    OpenAICompatibleTaskProvider,
)

app = FastAPI(title="ai-server", version="0.1.0")

CURRENT_PROTOCOL_VERSION = 2


def get_task_model_config() -> ModelProviderConfig:
    return ModelProviderConfig.from_env()


def get_task_provider(
    config: ModelProviderConfig = Depends(get_task_model_config),
) -> OpenAICompatibleTaskProvider:
    return OpenAICompatibleTaskProvider(config)


@app.get("/health")
def health(config: ModelProviderConfig = Depends(get_task_model_config)) -> dict:
    # process healthy != model ready: these two flags only report this
    # process's own static configuration, never a live check of the
    # provider - see model_provider.py's module docstring for why a
    # provider call never happens outside /dynamic-task itself.
    return {
        "status": "ok",
        "task_model_enabled": config.enabled,
        "task_model_configured": config.configured,
    }


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


# Milestone 2.13A2: /dynamic-task. Checks run in a fixed order - request
# size, protocol version, strict request schema, feature enabled, provider
# configured, provider call, strict draft schema, request-specific limits,
# response - so a request is only ever rejected for the first thing wrong
# with it, and nothing past "feature enabled" ever runs while the feature
# is off. Every failure past that point is fail-closed: a 5xx (or 400/413
# for a malformed/oversized/wrong-version request), never a synthesized
# QuestProposalDraft. request_id/agent_id/snapshot_sequence in the
# response always come from the original request/context, never from the
# model - the same "client's own echo, never server's claim" rule
# /decision already follows.
@app.post("/dynamic-task", response_model=DynamicTaskResponse)
async def dynamic_task(
    raw_request: Request,
    config: ModelProviderConfig = Depends(get_task_model_config),
    provider: OpenAICompatibleTaskProvider = Depends(get_task_provider),
) -> DynamicTaskResponse:
    # Streamed and bounded on purpose: awaiting raw_request.body() would
    # buffer the whole request in memory before max_request_bytes is ever
    # checked, so a byte cap enforced only after a full read is a
    # validation limit, not a resource bound. Reading via .stream() lets
    # us reject as soon as the running total crosses the limit, without
    # ever materializing more than max_request_bytes (plus at most one
    # chunk) of the request.
    body = bytearray()
    async for chunk in raw_request.stream():
        body.extend(chunk)
        if len(body) > config.max_request_bytes:
            raise HTTPException(
                status_code=413,
                detail=f"request body exceeded {config.max_request_bytes} bytes",
            )
    body_bytes = bytes(body)

    try:
        raw_payload = json.loads(body_bytes)
    except json.JSONDecodeError as exc:
        raise HTTPException(status_code=400, detail="request body was not valid JSON") from exc

    if not isinstance(raw_payload, dict) or raw_payload.get("protocol_version") != CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION:
        raise HTTPException(
            status_code=400,
            detail=f"unsupported protocol_version, expected {CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION}",
        )

    try:
        task_request = DynamicTaskRequest.model_validate(raw_payload)
    except ValidationError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    if not config.enabled:
        raise HTTPException(status_code=503, detail="dynamic-task model generation is disabled")
    if not config.configured:
        raise HTTPException(status_code=503, detail="dynamic-task model provider is not configured")

    try:
        draft = await provider.generate(task_request)
    except ModelProviderTimeout as exc:
        raise HTTPException(status_code=504, detail=str(exc)) from exc
    except (
        ModelProviderBadStatus,
        ModelProviderUnavailable,
        ModelProviderOversizedResponse,
        ModelProviderMalformedContent,
    ) as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    try:
        validate_draft_against_context(draft, task_request.context)
    except QuestProposalPolicyError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    return DynamicTaskResponse(
        protocol_version=CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION,
        request_id=task_request.request_id,
        agent_id=task_request.context.agent_id,
        snapshot_sequence=task_request.context.snapshot_sequence,
        proposal=draft,
    )
