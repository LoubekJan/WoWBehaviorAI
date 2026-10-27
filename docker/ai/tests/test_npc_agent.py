"""NPC decisions are tested against fake providers and bounded mock HTTP only."""
import asyncio
import copy
import json
import unittest
from dataclasses import replace
from unittest.mock import patch

import httpx
from fastapi.testclient import TestClient

from app.main import app
from app.model_provider import (
    ModelProviderBadStatus,
    ModelProviderConfig,
    ModelProviderMalformedContent,
    ModelProviderOversizedResponse,
    ModelProviderTimeout,
    ModelProviderUnavailable,
    OpenAICompatibleTaskProvider,
)
from app.npc_agent import MAX_REQUEST_BYTES, get_npc_config, get_npc_provider

CONFIG = ModelProviderConfig(True, "http://model.invalid/v1/chat/completions", "local", 1000, 32768, 8192, 512)
DECISION = {"action_token": 5, "speech": "Dobrý den, poutníku.", "speech_target": 7, "emote": "WAVE"}
REQUEST = {
    "protocol_version": 1, "request_id": 9, "agent_id": 42, "snapshot_sequence": 123,
    "context": {
        "profile": {"name": "Mira", "backstory": "A farmer who moved to Goldshire.\nShe misses her sister.",
            "personality": "Patient and curious."},
        "needs": {"health_pressure": 0.0, "hunger": 0.6, "fatigue": 0.2,
            "safety_pressure": 0.1, "resource_pressure": 0.0},
        "self": {"map_id": 0, "position": {"x": -100.0, "y": 20.0, "z": 2.0, "orientation": 0.5},
            "health": 100, "max_health": 100, "alive": True, "in_combat": False,
            "current_action": "WAIT", "last_action_result": "WAIT_COMPLETED"},
        "recent_dialogue": [{"speaker": "Poutník", "text": "Dobrý den!", "kind": "PLAYER"},
            {"speaker": "Tomas", "text": "Dinner is ready.", "kind": "NPC"}],
        "nearby": [{"token": 7, "name": "Poutník", "kind": "PLAYER", "distance": 3.0},
            {"token": 8, "name": "Tomas", "kind": "NPC", "distance": 6.0}],
        "actions": [{"token": 5, "type": "MOVE_TO", "description": "Walk to the dinner table."},
            {"token": 6, "type": "REST", "description": "Rest here."}],
        "emotes": ["NONE", "WAVE", "TALK"], "speech_targets": [7, 8],
    },
}


class Provider:
    def __init__(self, content=None):
        self.content = json.dumps(DECISION, ensure_ascii=False) if content is None else content
        self.calls = []

    async def complete(self, prompt, context):
        self.calls.append((prompt, json.loads(context)))
        if isinstance(self.content, Exception):
            raise self.content
        return self.content


class NpcAgentTests(unittest.TestCase):
    def setUp(self):
        self.provider = Provider()
        app.dependency_overrides[get_npc_config] = lambda: CONFIG
        app.dependency_overrides[get_npc_provider] = lambda: self.provider
        self.client = TestClient(app)

    def tearDown(self):
        self.client.close()
        app.dependency_overrides.clear()

    def post(self, payload=None):
        return self.client.post("/npc-agent", content=json.dumps(REQUEST if payload is None else payload),
            headers={"content-type": "application/json"})

    def test_returns_offered_decision_with_original_envelope(self):
        response = self.post()
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json(), {"protocol_version": 1, "request_id": 9,
            "agent_id": 42, "snapshot_sequence": 123, "decision": DECISION})
        prompt, context = self.provider.calls[0]
        self.assertEqual(context, REQUEST["context"])
        self.assertNotIn("agent_id", context)
        self.assertIn("Czech", prompt)
        self.assertIn("last_action_result", prompt)

    def test_decline_silence_and_public_speech_are_explicit_model_choices(self):
        for speech in ("", "Good evening."):
            decision = {"action_token": 0, "speech": speech, "speech_target": 0, "emote": "NONE"}
            self.provider.content = json.dumps(decision)
            self.assertEqual(self.post().json()["decision"], decision)

    def test_can_address_another_npc(self):
        self.provider.content = json.dumps(dict(DECISION, speech_target=8, speech="Thank you, Tomas."))
        self.assertEqual(self.post().json()["decision"]["speech_target"], 8)

    def test_can_choose_offered_flight_from_an_observed_threat(self):
        request = copy.deepcopy(REQUEST)
        request["context"]["needs"]["safety_pressure"] = 0.9
        request["context"]["actions"].append({"token": 10, "type": "FLEE",
            "description": "Flee from the visible wolf threatening you."})
        self.provider.content = json.dumps(dict(DECISION, action_token=10, speech="", speech_target=0, emote="NONE"))
        response = self.post(request)
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()["decision"]["action_token"], 10)
        self.assertEqual(self.provider.calls[0][1]["actions"][-1]["type"], "FLEE")
        self.assertEqual(self.post().status_code, 502)

    def test_conversations_and_lore_do_not_leak_between_requests(self):
        self.post()
        request = copy.deepcopy(REQUEST)
        request["agent_id"] = 43
        request["context"]["profile"] = {"name": "Second", "backstory": "Unrelated life.", "personality": "Shy."}
        request["context"]["recent_dialogue"] = []
        self.assertEqual(self.post(request).status_code, 200)
        self.assertEqual(self.provider.calls[1][1], request["context"])
        self.assertNotIn("Mira", json.dumps(self.provider.calls[1][1]))

    def test_invalid_decisions_fail_closed(self):
        mutations = [
            {"action_token": 1}, {"action_token": True}, {"action_token": "5"}, {"action_token": 5.0},
            {"speech_target": 9}, {"speech_target": -1}, {"emote": "LAUGH"}, {"emote": "DANCE"},
            {"speech": "x" * 257}, {"speech": "😀" * 193}, {"speech": "hello\nworld"},
            {"speech": "hello\x00world"}, {"speech": "hello\u200bworld"}, {"speech": "|cffff0000red|r"},
            {"speech": "|Hitem:1|h[item]|h"}, {"speech": 7}, {"speech": "\ud800"},
            {"extra": "ignored?"}, {"action_token": 2**32},
        ]
        for fields in mutations:
            with self.subTest(fields=fields):
                self.provider.content = json.dumps(dict(DECISION, **fields))
                response = self.post()
                self.assertEqual(response.status_code, 502, response.text)
                self.assertNotIn("decision", response.json())
        for content in ("null", "[]", "not json", "[" * 1500 + "0" + "]" * 1500,
            '{"action_token":5,"action_token":0}',
            json.dumps(DECISION).replace('"action_token": 5', '"action_token": NaN'),
            json.dumps({key: value for key, value in DECISION.items() if key != "speech"})):
            with self.subTest(content=content):
                self.provider.content = content
                self.assertEqual(self.post().status_code, 502)

    def test_speech_utf8_and_character_limits_accept_exact_boundaries(self):
        for speech in ("a" * 256, "界" * 256, "😀" * 192):
            with self.subTest(speech=speech[:2]):
                self.provider.content = json.dumps(dict(DECISION, speech=speech))
                self.assertEqual(self.post().status_code, 200)

    def test_invalid_requests_never_call_model(self):
        mutations = [
            (("protocol_version",), True), (("protocol_version",), 2), (("request_id",), 0),
            (("agent_id",), 2**64), (("snapshot_sequence",), "123"), (("extra",), "bad"),
            (("context", "extra"), "bad"), (("context", "profile", "name"), "x" * 81),
            (("context", "profile", "name"), "Mira\nOther"), (("context", "profile", "name"), "|Hfoo"),
            (("context", "profile", "backstory"), "界" * 2731),
            (("context", "profile", "personality"), "界" * 683),
            (("context", "needs", "hunger"), 1.1), (("context", "needs", "hunger"), "0.4"),
            (("context", "self", "alive"), 1), (("context", "self", "health"), -1),
            (("context", "self", "position", "x"), float("nan")),
            (("context", "self", "last_action_result"), "x" * 129),
            (("context", "recent_dialogue"), REQUEST["context"]["recent_dialogue"] * 7),
            (("context", "recent_dialogue", 0, "speaker"), "x" * 81),
            (("context", "recent_dialogue", 0, "speaker"), "player\tname"),
            (("context", "recent_dialogue", 0, "text"), "😀" * 193),
            (("context", "nearby", 0, "name"), "x" * 81),
            (("context", "nearby", 0, "distance"), -1.0),
            (("context", "actions"), []), (("context", "actions", 0, "type"), "TELEPORT"),
            (("context", "actions", 0, "token"), True),
            (("context", "actions", 0, "description"), "x" * 257),
            (("context", "emotes"), ["NONE", "DANCE"]),
            (("context", "speech_targets"), [True]), (("context", "speech_targets"), [9]),
        ]
        for path, value in mutations:
            with self.subTest(path=path, value=str(value)[:50]):
                request = copy.deepcopy(REQUEST)
                parent = request
                for part in path[:-1]:
                    parent = parent[part]
                parent[path[-1]] = value
                response = self.post(request)
                self.assertEqual(response.status_code, 422, response.text)
        self.assertEqual(self.provider.calls, [])

    def test_collection_bounds_and_duplicate_tokens(self):
        for collection in ("actions", "nearby", "speech_targets", "emotes"):
            with self.subTest(collection=collection):
                request = copy.deepcopy(REQUEST)
                request["context"][collection].append(copy.deepcopy(request["context"][collection][0]))
                self.assertEqual(self.post(request).status_code, 422)
        for collection, count in (("nearby", 17), ("actions", 33), ("speech_targets", 17)):
            with self.subTest(collection=collection, count=count):
                request = copy.deepcopy(REQUEST)
                request["context"][collection] = [copy.deepcopy(request["context"][collection][0]) for _ in range(count)]
                self.assertEqual(self.post(request).status_code, 422)
        self.assertEqual(self.provider.calls, [])

    def test_duplicate_json_keys_invalid_utf8_and_oversize(self):
        duplicate = json.dumps(REQUEST).replace('"request_id": 9', '"request_id": 9,"request_id": 10')
        nested = json.dumps(REQUEST).replace('"hunger": 0.6', '"hunger": 0.6,"hunger": 0.9')
        for body in (duplicate, nested, b"\xff", "null", "[]", "not JSON", "[" * 1500 + "0" + "]" * 1500):
            with self.subTest(body=str(body)[:40]):
                self.assertEqual(self.client.post("/npc-agent", content=body).status_code, 422)
        self.assertEqual(self.client.post("/npc-agent", content=" " * (MAX_REQUEST_BYTES + 1)).status_code, 413)
        self.assertEqual(self.provider.calls, [])

    def test_disabled_or_unconfigured(self):
        for config in (replace(CONFIG, enabled=False), replace(CONFIG, url=""), replace(CONFIG, model_name="")):
            app.dependency_overrides[get_npc_config] = lambda: config
            self.assertEqual(self.post().status_code, 503)
        self.assertEqual(self.provider.calls, [])

    def test_provider_failures_have_no_fallback_decision(self):
        for error, status in ((ModelProviderTimeout("private detail"), 504),
            (ModelProviderUnavailable("private detail"), 502), (ModelProviderBadStatus(500), 502),
            (ModelProviderMalformedContent("private detail"), 502),
            (ModelProviderOversizedResponse("private detail"), 502)):
            with self.subTest(error=type(error).__name__):
                self.provider.content = error
                response = self.post()
                self.assertEqual(response.status_code, status)
                self.assertNotIn("decision", response.json())
                self.assertNotIn("private detail", response.text)

    def test_real_mock_transport_receives_profile_needs_and_dialogue(self):
        calls = []
        def model(request):
            calls.append(json.loads(request.content))
            return httpx.Response(200, json={"choices": [{"message": {"content": json.dumps(DECISION)}}]})
        self.provider = OpenAICompatibleTaskProvider(CONFIG, httpx.MockTransport(model))
        self.assertEqual(self.post().status_code, 200)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["max_tokens"], 512)
        self.assertEqual(calls[0]["response_format"], {"type": "json_object"})
        self.assertEqual(json.loads(calls[0]["messages"][1]["content"]), REQUEST["context"])

    def test_real_transport_response_limit_and_malformed_content(self):
        for response in (httpx.Response(200, content=b" " * 8193),
            httpx.Response(200, json={"choices": []}), httpx.Response(503, text="unavailable")):
            with self.subTest(status=response.status_code, size=len(response.content)):
                self.provider = OpenAICompatibleTaskProvider(CONFIG, httpx.MockTransport(lambda _request: response))
                self.assertEqual(self.post().status_code, 502)

    def test_health_reports_only_static_npc_configuration(self):
        with patch("app.main.get_npc_config", return_value=CONFIG):
            health = self.client.get("/health").json()
        self.assertTrue(health["npc_model_enabled"])
        self.assertTrue(health["npc_model_configured"])
        self.assertEqual(self.provider.calls, [])


class NpcConfigurationTests(unittest.TestCase):
    def test_independent_enablement_and_provider_fallback(self):
        with patch.dict("os.environ", {"AI_TASK_MODEL_ENABLED": "0", "AI_TASK_MODEL_URL": CONFIG.url,
            "AI_TASK_MODEL_NAME": "shared", "AI_TASK_MODEL_API_KEY": "private-test-key",
            "AI_NPC_MODEL_ENABLED": "1"}, clear=True):
            config = get_npc_config()
        self.assertTrue(config.enabled and config.configured)
        self.assertEqual(config.model_name, "shared")
        self.assertEqual(config.api_key, "private-test-key")
        self.assertEqual((config.timeout_ms, config.max_tokens, config.max_request_bytes, config.max_response_bytes),
            (10000, 512, 32768, 8192))

    def test_optional_overrides_and_default_disabled(self):
        with patch.dict("os.environ", {"AI_NPC_MODEL_URL": CONFIG.url, "AI_NPC_MODEL_NAME": "dedicated",
            "AI_NPC_MODEL_API_KEY": "override-test-key"}, clear=True):
            config = get_npc_config()
        self.assertFalse(config.enabled)
        self.assertEqual(config.model_name, "dedicated")
        self.assertEqual(config.api_key, "override-test-key")

    def test_timeout_is_bounded_and_invalid_setting_uses_default(self):
        for setting, expected in (("-10", 1), ("1", 1), ("50000", 30000), ("invalid", 10000)):
            with self.subTest(setting=setting), patch.dict("os.environ", {"AI_NPC_MODEL_TIMEOUT_MS": setting}, clear=True):
                self.assertEqual(get_npc_config().timeout_ms, expected)


class NpcConcurrencyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncTearDown(self):
        app.dependency_overrides.clear()

    async def test_two_active_requests_reject_third_without_queueing(self):
        release = asyncio.Event()
        admitted = asyncio.Event()
        calls = []
        class SlowProvider:
            async def complete(self, prompt, context):
                calls.append(context)
                if len(calls) == 2:
                    admitted.set()
                await release.wait()
                return json.dumps(DECISION)
        app.dependency_overrides[get_npc_config] = lambda: CONFIG
        app.dependency_overrides[get_npc_provider] = lambda: SlowProvider()
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
            tasks = [asyncio.create_task(client.post("/npc-agent", json=REQUEST)) for _ in range(2)]
            try:
                await asyncio.wait_for(admitted.wait(), 2)
                response = await client.post("/npc-agent", json=REQUEST)
                self.assertEqual(response.status_code, 429)
                self.assertEqual(len(calls), 2)
            finally:
                release.set()
                responses = await asyncio.gather(*tasks)
            self.assertTrue(all(response.status_code == 200 for response in responses))
            self.assertEqual((await client.post("/npc-agent", json=REQUEST)).status_code, 200)

    async def test_total_model_deadline_cancels_provider_and_releases_slot(self):
        cancelled = asyncio.Event()
        class SlowProvider:
            async def complete(self, prompt, context):
                try:
                    await asyncio.Event().wait()
                finally:
                    cancelled.set()
        app.dependency_overrides[get_npc_config] = lambda: replace(CONFIG, timeout_ms=10)
        app.dependency_overrides[get_npc_provider] = lambda: SlowProvider()
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
            response = await client.post("/npc-agent", json=REQUEST)
            self.assertEqual(response.status_code, 504)
            self.assertTrue(cancelled.is_set())
            app.dependency_overrides[get_npc_provider] = lambda: Provider()
            self.assertEqual((await client.post("/npc-agent", json=REQUEST)).status_code, 200)
