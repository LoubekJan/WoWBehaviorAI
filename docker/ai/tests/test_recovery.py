import asyncio
import copy
import json
import unittest
from dataclasses import replace
from unittest.mock import patch

import httpx
from fastapi.testclient import TestClient

from app.main import app
from app.model_provider import ModelProviderConfig, OpenAICompatibleTaskProvider, ModelProviderTimeout
from app.recovery import get_recovery_config, get_recovery_provider

CONFIG = ModelProviderConfig(True, "http://model/v1/chat/completions", "local", 6000, 8192, 4096, 64)
REQUEST = {"protocol_version": 1, "request_id": 9, "agent_id": 80447, "episode": 123,
    "role": "GUARD", "problem": "RETURN_HOME", "failure": "RETURN_REPEATED_STEP", "hunger": 0.4,
    "home_distance": 30.0, "stalled_ms": 60000, "failures": 3,
    "options": [{"token": 2, "strategy": "DETOUR", "dx": 4.0, "dy": 3.0, "dz": 0.0,
        "distance": 5.0, "home_gain": -2.0, "nearby_prey": 0, "visits": 0, "successes": 0}]}

class Provider:
    def __init__(self, content='{"choice":2}'):
        self.content, self.calls = content, 0
    async def complete(self, prompt, context):
        self.calls += 1
        self.context = json.loads(context)
        if isinstance(self.content, Exception): raise self.content
        return self.content

class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.provider = Provider()
        app.dependency_overrides[get_recovery_config] = lambda: CONFIG
        app.dependency_overrides[get_recovery_provider] = lambda: self.provider
        self.client = TestClient(app)
    def tearDown(self):
        self.client.close()
        app.dependency_overrides.clear()
    def test_selects_only_offered_token_and_echoes_server_envelope(self):
        response = self.client.post('/recovery', json=REQUEST)
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json(), {"protocol_version": 1, "request_id": 9,
            "agent_id": 80447, "episode": 123, "choice": 2})
        self.assertNotIn('agent_id', self.provider.context)
        self.assertEqual(self.provider.context['options'], REQUEST['options'])
    def test_decline_is_not_synthesized_success(self):
        self.provider.content = '{"choice":0}'
        self.assertEqual(self.client.post('/recovery', json=REQUEST).json()['choice'], 0)
    def test_invalid_model_outputs_fail_closed(self):
        for content in ('{"choice":1}', '{"choice":true}', '{"choice":"2"}', '{"choice":2.0}',
            '{"choice":2,"x":4}', '{"choice":2,"choice":0}', '{"choice":-1}',
            '{"choice":9}', 'null', '[]', 'not json', '{"choice":NaN}'):
            with self.subTest(content=content):
                self.provider.content = content
                self.assertEqual(self.client.post('/recovery', json=REQUEST).status_code, 502)
    def test_request_bounds_and_types_before_model(self):
        for field, value in [('options', []), ('options', REQUEST['options']*2),
            ('protocol_version', True), ('protocol_version', 2), ('agent_id', -1),
            ('episode', 2**64), ('failures', '3'), ('hunger', 2), ('problem', 'TELEPORT')]:
            with self.subTest(field=field, value=value):
                request = copy.deepcopy(REQUEST); request[field] = value
                self.assertEqual(self.client.post('/recovery', json=request).status_code, 422)
        self.assertEqual(self.provider.calls, 0)
    def test_duplicate_request_keys(self):
        body = json.dumps(REQUEST).replace('"request_id": 9', '"request_id": 9,"request_id": 10')
        self.assertEqual(self.client.post('/recovery', content=body).status_code, 422)
        self.assertEqual(self.provider.calls, 0)
    def test_oversized_request(self):
        self.assertEqual(self.client.post('/recovery', content=' '*8193).status_code, 413)
        self.assertEqual(self.provider.calls, 0)
    def test_disabled_or_unconfigured(self):
        for config in (replace(CONFIG, enabled=False), replace(CONFIG, url='')):
            app.dependency_overrides[get_recovery_config] = lambda: config
            self.assertEqual(self.client.post('/recovery', json=REQUEST).status_code, 503)
        self.assertEqual(self.provider.calls, 0)
    def test_timeout_has_no_fallback_choice(self):
        self.provider.content = ModelProviderTimeout('timeout')
        response = self.client.post('/recovery', json=REQUEST)
        self.assertEqual(response.status_code, 504)
        self.assertNotIn('choice', response.json())
    def test_reuses_backend_without_enabling_quests(self):
        with patch.dict('os.environ', {'AI_TASK_MODEL_ENABLED':'0', 'AI_TASK_MODEL_URL':CONFIG.url,
            'AI_TASK_MODEL_NAME':'local', 'AI_RECOVERY_MODEL_ENABLED':'1', 'AI_RECOVERY_MODEL_URL':'',
            'AI_RECOVERY_MODEL_NAME':''}):
            config = get_recovery_config()
            self.assertTrue(config.enabled and config.configured)
            self.assertEqual(config.max_tokens, 64)
    def test_real_provider_transport_receives_recovery_prompt(self):
        calls = []
        def model(request):
            calls.append(json.loads(request.content))
            return httpx.Response(200, json={'choices':[{'message':{'content':'{"choice":2}'}}]})
        self.provider = OpenAICompatibleTaskProvider(CONFIG, httpx.MockTransport(model))
        self.assertEqual(self.client.post('/recovery', json=REQUEST).status_code, 200)
        self.assertEqual(calls[0]['max_tokens'], 64)
        self.assertIn('recovery option', calls[0]['messages'][0]['content'])

class CapacityTests(unittest.IsolatedAsyncioTestCase):
    async def test_third_request_is_rejected_without_waiting_for_model(self):
        release = asyncio.Event(); admitted = asyncio.Event(); calls = []
        class SlowProvider:
            async def complete(self, prompt, context):
                calls.append(context)
                if len(calls) == 2: admitted.set()
                await release.wait()
                return '{"choice":2}'
        app.dependency_overrides[get_recovery_config] = lambda: CONFIG
        app.dependency_overrides[get_recovery_provider] = lambda: SlowProvider()
        try:
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url='http://test') as client:
                tasks = [asyncio.create_task(client.post('/recovery', json=REQUEST)) for _ in range(2)]
                try:
                    await asyncio.wait_for(admitted.wait(), 2)
                    response = await client.post('/recovery', json=REQUEST)
                    self.assertEqual(response.status_code, 429)
                    self.assertEqual(len(calls), 2)
                finally:
                    release.set()
                    responses = await asyncio.gather(*tasks)
                self.assertTrue(all(r.status_code == 200 for r in responses))
        finally:
            app.dependency_overrides.clear()
