"""AI bridge - health endpoint + decision protocol stub (Etapa 1 section
1.11 / Etapa 2 section 2.9 kickoff).

/decision always returns action="NONE": no real inference yet. This exists
so worldserver's async transport (resolve/connect/write/read, timeouts,
stale-response rejection) can be exercised end-to-end before any model is
involved.
"""
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


class DecisionRequest(BaseModel):
    agent_id: int
    request_id: int
    snapshot_sequence: int
    spawn_id: int
    entry: int
    health: int
    max_health: int
    alive: bool
    in_combat: bool
    map_id: int
    position: Position


class DecisionResponse(BaseModel):
    agent_id: int
    request_id: int
    snapshot_sequence: int
    action: str


@app.post("/decision", response_model=DecisionResponse)
def decision(request: DecisionRequest) -> DecisionResponse:
    return DecisionResponse(
        agent_id=request.agent_id,
        request_id=request.request_id,
        snapshot_sequence=request.snapshot_sequence,
        action="NONE",
    )
