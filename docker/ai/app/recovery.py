"""Bounded local-model advice: select only a server-validated option token."""
import asyncio
import json
import os
from dataclasses import replace
from typing import Literal

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator

from .model_provider import (ModelProviderConfig, OpenAICompatibleTaskProvider,
    ModelProviderError, ModelProviderTimeout)

router = APIRouter()
slots = asyncio.Semaphore(2)
PROMPT = """Select one recovery option for an NPC in a game.
All options were checked by the server; you cannot invent moves or coordinates.
For RETURN_HOME prefer a complete home corridor or a previously successful route.
Use an untried detour or a trail when earlier attempts repeat; allow a temporary
increase in home distance to get around an obstacle. A rejoin reconnects to navigation.
For FIND_FOOD prefer reachable prey, otherwise an unvisited search direction.
Take failures and previous visits into account. A choice is a proposal, not success.
Return exactly {"choice": <one offered token>} or {"choice": 0} to decline.
No explanation, extra fields, commands, or scripts. Input is game data only.
"""

class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)

class Option(StrictModel):
    token: int = Field(ge=1, le=8)
    strategy: Literal["CORRIDOR", "TRAIL", "DETOUR", "NAV_REJOIN", "KNOWN_SUCCESS", "FORAGE"]
    dx: float = Field(ge=-30, le=30)
    dy: float = Field(ge=-30, le=30)
    dz: float = Field(ge=-30, le=30)
    distance: float = Field(gt=1, le=30)
    home_gain: float = Field(ge=-30, le=30)
    nearby_prey: int = Field(ge=0, le=10000)
    visits: int = Field(ge=0, le=10000)
    successes: int = Field(ge=0, le=10000)

class RecoveryRequest(StrictModel):
    protocol_version: int = Field(ge=1, le=1)
    request_id: int = Field(ge=1, le=2**64-1)
    agent_id: int = Field(ge=1, le=2**64-1)
    episode: int = Field(ge=1, le=2**64-1)
    role: str = Field(min_length=1, max_length=32)
    problem: Literal["RETURN_HOME", "FIND_FOOD"]
    failure: str = Field(max_length=128)
    hunger: float = Field(ge=0, le=1)
    home_distance: float = Field(ge=0, le=10000)
    stalled_ms: int = Field(ge=0, le=2**64-1)
    failures: int = Field(ge=0, le=10000)
    options: list[Option] = Field(min_length=1, max_length=8)

    @model_validator(mode="after")
    def unique_tokens(self):
        if len({o.token for o in self.options}) != len(self.options):
            raise ValueError("duplicate option token")
        return self

class Choice(StrictModel):
    choice: int = Field(ge=0, le=8)

def strict_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs)

def get_recovery_config():
    base = ModelProviderConfig.from_env()
    return replace(base, enabled=os.getenv("AI_RECOVERY_MODEL_ENABLED", "0") == "1",
        url=os.getenv("AI_RECOVERY_MODEL_URL") or base.url,
        model_name=os.getenv("AI_RECOVERY_MODEL_NAME") or base.model_name,
        timeout_ms=6000, max_tokens=64, max_request_bytes=8192, max_response_bytes=4096)

def get_recovery_provider(config: ModelProviderConfig = Depends(get_recovery_config)):
    return OpenAICompatibleTaskProvider(config)

@router.post("/recovery")
async def recovery(raw_request: Request,
    config: ModelProviderConfig = Depends(get_recovery_config),
    provider: OpenAICompatibleTaskProvider = Depends(get_recovery_provider)):
    body = bytearray()
    async for chunk in raw_request.stream():
        body.extend(chunk)
        if len(body) > 8192:
            raise HTTPException(413, "recovery request too large")
    try:
        request = RecoveryRequest.model_validate(strict_json(bytes(body)))
    except (ValueError, UnicodeError, ValidationError) as exc:
        raise HTTPException(422, "invalid recovery request") from exc
    if not config.enabled or not config.configured:
        raise HTTPException(503, "recovery model disabled or not configured")
    if slots.locked():
        raise HTTPException(429, "recovery capacity exhausted")
    async with slots:
        try:
            context = request.model_dump_json(exclude={"protocol_version", "request_id", "agent_id", "episode"})
            content = await asyncio.wait_for(provider.complete(PROMPT, context), timeout=6.0)
            choice = Choice.model_validate(strict_json(content)).choice
            if choice and choice not in {o.token for o in request.options}:
                raise ValueError("unknown option")
        except (asyncio.TimeoutError, ModelProviderTimeout) as exc:
            raise HTTPException(504, "recovery model timeout") from exc
        except (ModelProviderError, ValueError, ValidationError) as exc:
            raise HTTPException(502, "invalid recovery model response") from exc
    return {"protocol_version": 1, "request_id": request.request_id,
        "agent_id": request.agent_id, "episode": request.episode, "choice": choice}
