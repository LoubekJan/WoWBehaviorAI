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
carries untrusted or potentially sensitive text); there is currently no
API-key configuration (typical local OpenAI-compatible backends don't
require one), and if one is ever added it must follow the same rule.
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
    "You generate an untrusted dynamic quest draft.\n\n"
    "Return JSON only.\n\n"
    "You may only use target_token values present in candidate_targets.\n"
    "You may only use supported objective types.\n"
    "Stay within every limit supplied by the server.\n\n"
    "Do not generate SQL, commands, scripts, GUIDs, spawn IDs, spells,\n"
    "actions, world mutations or completion claims."
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

    @property
    def configured(self) -> bool:
        return bool(self.url) and bool(self.model_name)

    @classmethod
    def from_env(cls) -> "ModelProviderConfig":
        return cls(
            enabled=os.environ.get("AI_TASK_MODEL_ENABLED", "0") == "1",
            url=os.environ.get("AI_TASK_MODEL_URL", ""),
            model_name=os.environ.get("AI_TASK_MODEL_NAME", ""),
            timeout_ms=int(os.environ.get("AI_TASK_MODEL_TIMEOUT_MS", "5000")),
            max_request_bytes=int(os.environ.get("AI_TASK_MODEL_MAX_REQUEST_BYTES", "32768")),
            max_response_bytes=int(os.environ.get("AI_TASK_MODEL_MAX_RESPONSE_BYTES", "16384")),
            max_tokens=int(os.environ.get("AI_TASK_MODEL_MAX_TOKENS", "512")),
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

        timeout = httpx.Timeout(self._config.timeout_ms / 1000)

        try:
            async with httpx.AsyncClient(timeout=timeout, transport=self._transport) as client:
                response = await client.post(self._config.url, json=payload)
        except httpx.TimeoutException as exc:
            raise ModelProviderTimeout("model provider request timed out") from exc
        except httpx.HTTPError as exc:
            raise ModelProviderUnavailable("model provider unreachable") from exc

        if not (200 <= response.status_code < 300):
            logger.warning("model provider returned HTTP %s", response.status_code)
            raise ModelProviderBadStatus(response.status_code)

        if len(response.content) > self._config.max_response_bytes:
            raise ModelProviderOversizedResponse(
                f"model provider response exceeded {self._config.max_response_bytes} bytes"
            )

        content = _extract_message_content(response)

        try:
            draft_payload = json.loads(content)
        except json.JSONDecodeError as exc:
            raise ModelProviderMalformedContent("model output was not valid JSON") from exc

        try:
            return QuestProposalDraft.model_validate(draft_payload)
        except ValidationError as exc:
            raise ModelProviderMalformedContent("model output failed draft schema validation") from exc


def _extract_message_content(response: httpx.Response) -> str:
    try:
        body = response.json()
        return body["choices"][0]["message"]["content"]
    except (json.JSONDecodeError, KeyError, IndexError, TypeError) as exc:
        raise ModelProviderMalformedContent(
            "model provider response did not match the chat-completions shape"
        ) from exc
