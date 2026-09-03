"""Milestone 2.13A2: a single OpenAI-compatible chat-completions call for
dynamic-task generation.

Inference deliberately lives here, not in main.py's endpoint handler -
main.py owns the /dynamic-task trust boundary (request size, protocol
version, strict request/draft schema, request-specific limits), this
module owns exactly one thing: turning a validated DynamicTaskRequest
into a QuestProposalDraft by calling a configured backend (Ollama,
llama.cpp's server, or any other OpenAI-compatible endpoint - the
provider is swappable by URL, main.py never changes for a different
backend).

Fail-closed by construction: at most one HTTP request, an explicit
timeout, no automatic retry, and no fallback draft on any failure - every
failure mode raises a ModelProviderError subclass that main.py turns into
a 5xx. A provider failure must never become a synthesized
QuestProposalDraft. The request/response body is never logged in full (it
carries untrusted or potentially sensitive text). AI_TASK_MODEL_API_KEY
is optional (many local OpenAI-compatible backends don't require one) -
when set, it is sent as a bearer token and never logged or included in
any exception message. AI_TASK_MODEL_ENABLE_THINKING is also optional and
defaults to "0" (explicitly disable a reasoning backend's chain-of-thought)
for this development phase - see ModelProviderConfig.enable_thinking's
own comment for the tri-state mechanism and how to get the "send nothing
backend-specific at all" behavior back.
"""
from __future__ import annotations

import json
import logging
import os
from dataclasses import dataclass
from typing import Optional

import httpx
from pydantic import ValidationError

from .dynamic_task import DynamicTaskRequest, QuestProposalDraft

logger = logging.getLogger("ai-server.model_provider")

SYSTEM_PROMPT = (
    "You generate one untrusted dynamic quest draft.\n\n"
    "Return exactly one JSON object and nothing else.\n\n"
    "The JSON object MUST contain exactly these fields:\n"
    "{\n"
    '  "objective": "KILL_CREATURE",\n'
    '  "target_token": <integer>,\n'
    '  "required_count": <integer>,\n'
    '  "max_range_yards": <number>,\n'
    '  "expiry_ms": <integer>,\n'
    '  "reward_money_copper": <integer>,\n'
    '  "title": <string>,\n'
    '  "description": <string>\n'
    "}\n\n"
    "Rules:\n"
    "- Do not add any other fields.\n"
    '- objective must be exactly "KILL_CREATURE".\n'
    "- target_token must be one token from candidate_targets.\n"
    "- required_count must be greater than 0 and <= limits.max_required_count.\n"
    "- max_range_yards must be greater than 0 and <= limits.max_range_yards.\n"
    "- expiry_ms must be greater than 0 and <= limits.max_expiry_ms.\n"
    "- reward_money_copper must be >= 0 and <= limits.max_reward_money_copper.\n"
    "- title and description must be plain text.\n"
    "- Do not generate GUIDs, spawn IDs, SQL, commands, scripts, actions, "
    "world mutations, or completion claims."
)


class ModelProviderError(Exception):
    """Base class for every provider failure. Callers must fail closed -
    never invent a QuestProposalDraft when this (or a subclass) is raised.
    """


class ModelProviderTimeout(ModelProviderError):
    pass


class ModelProviderUnavailable(ModelProviderError):
    """The provider could not be reached at all (connection refused, DNS
    failure, TLS error, ...) - distinct from a timeout, which means it was
    reached but didn't answer in time.
    """


class ModelProviderBadStatus(ModelProviderError):
    def __init__(self, status_code: int) -> None:
        super().__init__(f"model provider returned HTTP {status_code}")
        self.status_code = status_code


class ModelProviderOversizedResponse(ModelProviderError):
    pass


class ModelProviderMalformedContent(ModelProviderError):
    """The provider answered in time with a 2xx, but its body didn't match
    the OpenAI chat-completions shape, its message content wasn't valid
    JSON, or that JSON didn't validate as a QuestProposalDraft.
    """


@dataclass(frozen=True)
class ModelProviderConfig:
    enabled: bool
    url: str
    model_name: str
    timeout_ms: int
    max_request_bytes: int
    max_response_bytes: int
    max_tokens: int
    # Optional - most local OpenAI-compatible backends (Ollama, llama.cpp's
    # server) don't require one, but some (including proxies/gateways in
    # front of one) do. Never logged, never part of any exception message.
    api_key: str = ""
    # Tri-state, deliberately not a plain bool: None means "don't send
    # chat_template_kwargs at all" - a llama.cpp-specific parameter that a
    # backend without that template kwarg could reject or silently ignore.
    # True/False (AI_TASK_MODEL_ENABLE_THINKING=1/0) sends it explicitly,
    # e.g. to force a reasoning model's chain-of-thought off so it spends
    # its max_tokens budget on the actual JSON answer instead (see
    # SYSTEM_PROMPT's own history). from_env()'s own default is "0", not
    # unset - see its comment for why - so the type-level default here
    # (None) only matters for a caller constructing this dataclass
    # directly rather than through from_env().
    enable_thinking: Optional[bool] = None

    @property
    def configured(self) -> bool:
        return bool(self.url) and bool(self.model_name)

    @classmethod
    def from_env(cls) -> "ModelProviderConfig":
        # Milestone 2.13A3B development default: "0" (explicitly send
        # enable_thinking=false), not "" (send nothing) - the real local
        # model this phase of development is being validated against is a
        # reasoning model, and this project's own SYSTEM_PROMPT/token
        # budget only works reliably with its chain-of-thought turned
        # off. An operator who genuinely needs the tri-state "omit the
        # parameter entirely" behavior back can still set
        # AI_TASK_MODEL_ENABLE_THINKING= (empty) explicitly.
        enable_thinking_raw = os.environ.get("AI_TASK_MODEL_ENABLE_THINKING", "0")
        return cls(
            enabled=os.environ.get("AI_TASK_MODEL_ENABLED", "0") == "1",
            url=os.environ.get("AI_TASK_MODEL_URL", ""),
            model_name=os.environ.get("AI_TASK_MODEL_NAME", ""),
            timeout_ms=int(os.environ.get("AI_TASK_MODEL_TIMEOUT_MS", "30000")),
            max_request_bytes=int(os.environ.get("AI_TASK_MODEL_MAX_REQUEST_BYTES", "32768")),
            max_response_bytes=int(os.environ.get("AI_TASK_MODEL_MAX_RESPONSE_BYTES", "16384")),
            max_tokens=int(os.environ.get("AI_TASK_MODEL_MAX_TOKENS", "512")),
            api_key=os.environ.get("AI_TASK_MODEL_API_KEY", ""),
            enable_thinking=(
                None
                if enable_thinking_raw == ""
                else enable_thinking_raw == "1"
            ),
        )


class OpenAICompatibleTaskProvider:
    """One-shot client for a single configured OpenAI-compatible
    /v1/chat/completions endpoint. Holds no retry logic and no state
    across calls beyond its own static configuration.

    `transport` is a test-only seam (httpx.MockTransport) so unit tests
    can exercise the real request/parse/validate pipeline without a
    network call or a real model; production code never sets it and gets
    httpx's normal transport.
    """

    def __init__(self, config: ModelProviderConfig, transport: Optional[httpx.BaseTransport] = None) -> None:
        self._config = config
        self._transport = transport

    async def generate(self, request: DynamicTaskRequest) -> QuestProposalDraft:
        payload = {
            "model": self._config.model_name,
            "max_tokens": self._config.max_tokens,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": request.context.model_dump_json()},
            ],
            "response_format": {"type": "json_object"},
        }

        # Only added when explicitly configured (see enable_thinking's own
        # declaration comment) - a backend that doesn't recognize
        # chat_template_kwargs never sees it at all.
        if self._config.enable_thinking is not None:
            payload["chat_template_kwargs"] = {"enable_thinking": self._config.enable_thinking}

        timeout = httpx.Timeout(self._config.timeout_ms / 1000)

        # Only set when a key is actually configured - most local
        # OpenAI-compatible backends don't require one, and sending an
        # empty/placeholder Authorization header to one that doesn't want
        # it is needless surface area. The key itself never appears in a
        # log line or exception message anywhere in this module.
        headers = {}
        if self._config.api_key:
            headers["Authorization"] = f"Bearer {self._config.api_key}"

        # Streamed and bounded on purpose: client.post()/response.content
        # would buffer the entire body in memory before max_response_bytes
        # is ever checked, so a misbehaving or malicious backend could
        # force an unbounded allocation regardless of the configured
        # limit. Reading via client.stream()/aiter_bytes() lets us abort
        # as soon as the running total crosses the limit, without ever
        # materializing more than max_response_bytes (plus at most one
        # chunk) of the response.
        raw_body = bytearray()

        try:
            async with httpx.AsyncClient(timeout=timeout, transport=self._transport) as client:
                async with client.stream("POST", self._config.url, json=payload, headers=headers) as response:
                    if not (200 <= response.status_code < 300):
                        logger.warning("model provider returned HTTP %s", response.status_code)
                        raise ModelProviderBadStatus(response.status_code)

                    async for chunk in response.aiter_bytes():
                        raw_body.extend(chunk)
                        if len(raw_body) > self._config.max_response_bytes:
                            raise ModelProviderOversizedResponse(
                                f"model provider response exceeded {self._config.max_response_bytes} bytes"
                            )
        except httpx.TimeoutException as exc:
            raise ModelProviderTimeout("model provider request timed out") from exc
        except httpx.HTTPError as exc:
            raise ModelProviderUnavailable("model provider unreachable") from exc

        content = _extract_message_content(bytes(raw_body))

        try:
            draft_payload = json.loads(content)
        except json.JSONDecodeError as exc:
            raise ModelProviderMalformedContent("model output was not valid JSON") from exc

        try:
            return QuestProposalDraft.model_validate(draft_payload)
        except ValidationError as exc:
            raise ModelProviderMalformedContent("model output failed draft schema validation") from exc


def _extract_message_content(raw_body: bytes) -> str:
    try:
        body = json.loads(raw_body)
        content = body["choices"][0]["message"]["content"]
    except (json.JSONDecodeError, KeyError, IndexError, TypeError) as exc:
        raise ModelProviderMalformedContent(
            "model provider response did not match the chat-completions shape"
        ) from exc

    if not isinstance(content, str):
        raise ModelProviderMalformedContent("model provider message content was not a string")

    return content
