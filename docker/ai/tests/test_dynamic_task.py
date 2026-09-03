"""Milestone 2.13A2: /dynamic-task tests.

No real LLM anywhere in this file. Provider-level tests replace the
network with httpx.MockTransport so OpenAICompatibleTaskProvider's real
request/parse/validate code still runs end-to-end; endpoint-level tests
replace the whole provider with an in-process fake via FastAPI's
dependency_overrides, so main.py's own trust-boundary logic (ordering,
status-code mapping, never trusting model-supplied metadata) is what's
under test, not any model's wording. Nothing here asserts on generated
prose - every case is schema, bounds, ordering or fail-closed behavior.
"""
import json
import math
import unittest

import httpx
from fastapi.testclient import TestClient
from pydantic import ValidationError

from app.dynamic_task import (
    CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION,
    QUEST_CONTRACT_MAX_CANDIDATE_TARGETS,
    QUEST_CONTRACT_MAX_DESCRIPTION_LENGTH,
    QUEST_CONTRACT_MAX_RELEVANT_EVENTS,
    QUEST_CONTRACT_MAX_TITLE_LENGTH,
    UINT32_MAX,
    UINT64_MAX,
    DynamicTaskRequest,
    QuestContext,
    QuestObjectiveType,
    QuestProposalDraft,
    QuestProposalPolicyError,
    validate_draft_against_context,
)
from app.main import app, get_task_model_config, get_task_provider
from app.model_provider import (
    ModelProviderBadStatus,
    ModelProviderConfig,
    ModelProviderMalformedContent,
    ModelProviderOversizedResponse,
    ModelProviderTimeout,
    ModelProviderUnavailable,
    OpenAICompatibleTaskProvider,
)


def _valid_request_payload(**overrides) -> dict:
    payload = {
        "protocol_version": CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION,
        "request_id": 123,
        "context": {
            "agent_id": 42,
            "snapshot_sequence": 77,
            "problem": {
                "type": "CREATURE_KILLED",
                "actor_entry": 1001,
                "target_entry": 2002,
                "map_id": 0,
                "age_ms": 500,
            },
            "relevant_events": [],
            "candidate_targets": [
                {
                    "token": 1,
                    "entry": 2002,
                    "display_name": "Young Wolf",
                    "map_id": 0,
                    "distance_yards": 15.0,
                    "observation_age_ms": 250,
                }
            ],
            "limits": {
                "max_required_count": 10,
                "max_range_yards": 100.0,
                "max_expiry_ms": 600000,
                "max_reward_money_copper": 500,
            },
        },
    }
    payload.update(overrides)
    return payload


def _valid_task_request() -> DynamicTaskRequest:
    return DynamicTaskRequest.model_validate(_valid_request_payload())


def _valid_draft(**overrides) -> QuestProposalDraft:
    fields = dict(
        objective="KILL_CREATURE",
        target_token=1,
        required_count=3,
        max_range_yards=50.0,
        expiry_ms=300000,
        reward_money_copper=100,
        title="Cull the wolves",
        description="Thin the wolf pack near the road.",
    )
    fields.update(overrides)
    return QuestProposalDraft.model_validate(fields)


class _FakeProvider:
    """Endpoint-layer test double - never touches the network. `outcome`
    is either a QuestProposalDraft to return or an exception instance to
    raise, so tests can drive every main.py status-code mapping without a
    real provider call.
    """

    def __init__(self, outcome):
        self._outcome = outcome
        self.calls = 0

    async def generate(self, request):
        self.calls += 1
        if isinstance(self._outcome, Exception):
            raise self._outcome
        return self._outcome


def _provider_config(**overrides) -> ModelProviderConfig:
    fields = dict(
        enabled=True,
        url="http://fake-model.invalid/v1/chat/completions",
        model_name="fake-model",
        timeout_ms=1000,
        max_request_bytes=32768,
        max_response_bytes=16384,
        max_tokens=512,
    )
    fields.update(overrides)
    return ModelProviderConfig(**fields)


class DynamicTaskModelTests(unittest.TestCase):
    """Direct pydantic-model coverage: unknown fields, NaN/Infinity,
    objective allowlist and collection/text bounds are all enforced by the
    model classes themselves, independent of any endpoint or provider.
    """

    def test_extra_field_on_context_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["unexpected"] = "nope"
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_extra_field_on_request_rejected(self):
        payload = _valid_request_payload()
        payload["unexpected_field"] = "nope"
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_invalid_objective_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["objective"] = "SPAWN_NPC"
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_unknown_proposal_field_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["extra"] = "nope"
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_nan_literal_rejected(self):
        # json.loads accepts the non-standard NaN token by default - this
        # proves that token still can't survive into a QuestProposalDraft.
        raw = json.dumps(_valid_draft().model_dump(mode="json")).replace(
            '"max_range_yards": 50.0', '"max_range_yards": NaN'
        )
        parsed = json.loads(raw)
        self.assertTrue(math.isnan(parsed["max_range_yards"]))
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(parsed)

    def test_infinity_literal_rejected(self):
        raw = json.dumps(_valid_draft().model_dump(mode="json")).replace(
            '"reward_money_copper": 100', '"reward_money_copper": Infinity'
        )
        parsed = json.loads(raw)
        self.assertTrue(math.isinf(parsed["reward_money_copper"]))
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(parsed)

    def test_zero_required_count_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["required_count"] = 0
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_title_too_long_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["title"] = "x" * (QUEST_CONTRACT_MAX_TITLE_LENGTH + 1)
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_description_too_long_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["description"] = "x" * (QUEST_CONTRACT_MAX_DESCRIPTION_LENGTH + 1)
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_relevant_events_over_max_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["relevant_events"] = [
            {
                "type": "CREATURE_KILLED",
                "actor_entry": 1,
                "target_entry": 2,
                "importance": 0.5,
                "relevance": 0.5,
                "age_ms": 100,
            }
        ] * (QUEST_CONTRACT_MAX_RELEVANT_EVENTS + 1)
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_candidate_targets_over_max_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["candidate_targets"] = [
            {
                "token": i,
                "entry": 1,
                "display_name": "x",
                "map_id": 0,
                "distance_yards": 1.0,
                "observation_age_ms": 1,
            }
            for i in range(QUEST_CONTRACT_MAX_CANDIDATE_TARGETS + 1)
        ]
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_display_name_too_long_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["candidate_targets"][0]["display_name"] = "x" * 65
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    # -- strict=True: no lax-mode type coercion -----------------------

    def test_required_count_as_string_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["required_count"] = "3"
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_target_token_as_string_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["target_token"] = "1"
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_agent_id_as_string_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["agent_id"] = "42"
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_reward_money_copper_as_float_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["reward_money_copper"] = 100.0
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    # -- uint32/uint64 upper bounds ------------------------------------

    def test_agent_id_over_uint64_max_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["agent_id"] = UINT64_MAX + 1
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_request_id_over_uint64_max_rejected(self):
        payload = _valid_request_payload(request_id=UINT64_MAX + 1)
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)

    def test_target_token_over_uint32_max_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["target_token"] = UINT32_MAX + 1
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_required_count_over_uint32_max_rejected(self):
        payload = _valid_draft().model_dump(mode="json")
        payload["required_count"] = UINT32_MAX + 1
        with self.assertRaises(ValidationError):
            QuestProposalDraft.model_validate(payload)

    def test_map_id_over_uint32_max_rejected(self):
        payload = _valid_request_payload()
        payload["context"]["problem"]["map_id"] = UINT32_MAX + 1
        with self.assertRaises(ValidationError):
            DynamicTaskRequest.model_validate(payload)


class ValidateDraftAgainstContextTests(unittest.TestCase):
    """validate_draft_against_context() is the request-specific policy
    check main.py runs after a draft already passed schema validation -
    the model's own declared limits/candidate tokens for THIS request,
    not a static schema bound.
    """

    def setUp(self):
        self.context = QuestContext.model_validate(_valid_request_payload()["context"])

    def test_valid_draft_passes(self):
        validate_draft_against_context(_valid_draft(), self.context)

    def test_unknown_target_token_rejected(self):
        with self.assertRaises(QuestProposalPolicyError):
            validate_draft_against_context(_valid_draft(target_token=999), self.context)

    def test_required_count_over_limit_rejected(self):
        with self.assertRaises(QuestProposalPolicyError):
            validate_draft_against_context(_valid_draft(required_count=9999), self.context)

    def test_range_over_limit_rejected(self):
        with self.assertRaises(QuestProposalPolicyError):
            validate_draft_against_context(_valid_draft(max_range_yards=9999.0), self.context)

    def test_expiry_over_limit_rejected(self):
        with self.assertRaises(QuestProposalPolicyError):
            validate_draft_against_context(_valid_draft(expiry_ms=99999999), self.context)

    def test_reward_over_limit_rejected(self):
        with self.assertRaises(QuestProposalPolicyError):
            validate_draft_against_context(_valid_draft(reward_money_copper=999999), self.context)


class OpenAICompatibleTaskProviderTests(unittest.IsolatedAsyncioTestCase):
    """Exercises the real provider request/parse/validate pipeline against
    a mocked transport - no network, no real model, but genuine production
    code on the path from HTTP response to QuestProposalDraft.
    """

    def _provider(self, handler, **config_overrides) -> OpenAICompatibleTaskProvider:
        transport = httpx.MockTransport(handler)
        return OpenAICompatibleTaskProvider(_provider_config(**config_overrides), transport=transport)

    async def test_valid_response_returns_draft(self):
        draft_json = json.dumps(_valid_draft().model_dump(mode="json"))

        def handler(request):
            return httpx.Response(200, json={"choices": [{"message": {"content": draft_json}}]})

        provider = self._provider(handler)
        result = await provider.generate(_valid_task_request())
        self.assertEqual(result.objective, QuestObjectiveType.KILL_CREATURE)

    async def test_timeout_raises(self):
        def handler(request):
            raise httpx.TimeoutException("boom", request=request)

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderTimeout):
            await provider.generate(_valid_task_request())

    async def test_connection_error_raises_unavailable(self):
        def handler(request):
            raise httpx.ConnectError("boom", request=request)

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderUnavailable):
            await provider.generate(_valid_task_request())

    async def test_non_2xx_raises_bad_status(self):
        def handler(request):
            return httpx.Response(500, text="internal error")

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderBadStatus):
            await provider.generate(_valid_task_request())

    async def test_oversized_response_raises(self):
        draft_json = json.dumps(_valid_draft().model_dump(mode="json"))

        def handler(request):
            return httpx.Response(200, json={"choices": [{"message": {"content": draft_json}}]})

        provider = self._provider(handler, max_response_bytes=10)
        with self.assertRaises(ModelProviderOversizedResponse):
            await provider.generate(_valid_task_request())

    async def test_malformed_json_content_raises(self):
        def handler(request):
            return httpx.Response(200, json={"choices": [{"message": {"content": "not-json"}}]})

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderMalformedContent):
            await provider.generate(_valid_task_request())

    async def test_unexpected_response_shape_raises(self):
        def handler(request):
            return httpx.Response(200, json={"unexpected": "shape"})

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderMalformedContent):
            await provider.generate(_valid_task_request())

    async def test_draft_schema_invalid_raises(self):
        def handler(request):
            return httpx.Response(
                200,
                json={"choices": [{"message": {"content": json.dumps({"objective": "SPAWN_NPC"})}}]},
            )

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderMalformedContent):
            await provider.generate(_valid_task_request())

    async def test_null_message_content_raises(self):
        def handler(request):
            return httpx.Response(200, json={"choices": [{"message": {"content": None}}]})

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderMalformedContent):
            await provider.generate(_valid_task_request())

    async def test_object_message_content_raises(self):
        def handler(request):
            return httpx.Response(200, json={"choices": [{"message": {"content": {}}}]})

        provider = self._provider(handler)
        with self.assertRaises(ModelProviderMalformedContent):
            await provider.generate(_valid_task_request())


class DynamicTaskEndpointTests(unittest.TestCase):
    """FastAPI endpoint tests via dependency_overrides + _FakeProvider -
    covers ordering (disabled/unconfigured short-circuits before the
    provider is ever called), status-code mapping, and that response
    metadata always echoes the original request, never the model.
    """

    def setUp(self):
        self.client = TestClient(app)
        self.addCleanup(app.dependency_overrides.clear)

    def _install(self, *, enabled=True, configured=True, outcome=None, max_request_bytes=32768):
        config = _provider_config(
            enabled=enabled,
            url="http://fake-model.invalid/v1/chat/completions" if configured else "",
            model_name="fake-model" if configured else "",
            max_request_bytes=max_request_bytes,
        )
        provider = _FakeProvider(outcome if outcome is not None else _valid_draft())
        app.dependency_overrides[get_task_model_config] = lambda: config
        app.dependency_overrides[get_task_provider] = lambda: provider
        return provider

    def test_feature_disabled_returns_503_and_provider_not_called(self):
        provider = self._install(enabled=False)
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 503)
        self.assertEqual(provider.calls, 0)

    def test_provider_not_configured_returns_503_and_provider_not_called(self):
        provider = self._install(enabled=True, configured=False)
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 503)
        self.assertEqual(provider.calls, 0)

    def test_valid_request_returns_200_with_correct_metadata(self):
        self._install()
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body["request_id"], 123)
        self.assertEqual(body["agent_id"], 42)
        self.assertEqual(body["snapshot_sequence"], 77)
        self.assertEqual(body["proposal"]["objective"], "KILL_CREATURE")

    def test_wrong_protocol_version_rejected(self):
        self._install()
        response = self.client.post("/dynamic-task", json=_valid_request_payload(protocol_version=999))
        self.assertEqual(response.status_code, 400)

    def test_extra_request_field_rejected(self):
        provider = self._install()
        payload = _valid_request_payload()
        payload["unexpected_field"] = "nope"
        response = self.client.post("/dynamic-task", json=payload)
        self.assertEqual(response.status_code, 422)
        self.assertEqual(provider.calls, 0)

    def test_nan_literal_in_request_rejected(self):
        self._install()
        raw = json.dumps(_valid_request_payload()).replace(
            '"max_range_yards": 100.0', '"max_range_yards": NaN'
        )
        response = self.client.post(
            "/dynamic-task", content=raw.encode("utf-8"), headers={"content-type": "application/json"}
        )
        self.assertEqual(response.status_code, 422)

    def test_provider_timeout_maps_to_504(self):
        self._install(outcome=ModelProviderTimeout("boom"))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 504)

    def test_provider_bad_status_maps_to_502(self):
        self._install(outcome=ModelProviderBadStatus(500))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_provider_unavailable_maps_to_502(self):
        self._install(outcome=ModelProviderUnavailable("boom"))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_provider_malformed_content_maps_to_502(self):
        self._install(outcome=ModelProviderMalformedContent("boom"))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_provider_oversized_response_maps_to_502(self):
        self._install(outcome=ModelProviderOversizedResponse("boom"))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_target_token_not_in_candidates_maps_to_502(self):
        self._install(outcome=_valid_draft(target_token=999))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_required_count_exceeds_limit_maps_to_502(self):
        self._install(outcome=_valid_draft(required_count=9999))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_range_exceeds_limit_maps_to_502(self):
        self._install(outcome=_valid_draft(max_range_yards=9999.0))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_expiry_exceeds_limit_maps_to_502(self):
        self._install(outcome=_valid_draft(expiry_ms=99999999))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_reward_exceeds_limit_maps_to_502(self):
        self._install(outcome=_valid_draft(reward_money_copper=999999))
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 502)

    def test_oversized_request_returns_413_and_provider_not_called(self):
        provider = self._install(max_request_bytes=10)
        response = self.client.post("/dynamic-task", json=_valid_request_payload())
        self.assertEqual(response.status_code, 413)
        self.assertEqual(provider.calls, 0)

    def test_health_reports_config_without_claiming_ready(self):
        self._install(enabled=True, configured=True)
        response = self.client.get("/health")
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body["status"], "ok")
        self.assertTrue(body["task_model_enabled"])
        self.assertTrue(body["task_model_configured"])
        self.assertNotIn("model_ready", body)

    def test_health_default_disabled_unconfigured(self):
        self._install(enabled=False, configured=False)
        response = self.client.get("/health")
        body = response.json()
        self.assertFalse(body["task_model_enabled"])
        self.assertFalse(body["task_model_configured"])


if __name__ == "__main__":
    unittest.main()
