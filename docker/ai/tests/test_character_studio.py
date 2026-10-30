import asyncio
import copy
import json
import unittest

from fastapi.testclient import TestClient

from app.main import app
from app.character_studio import authoring_slot, get_authoring_config, get_authoring_provider
from app.model_provider import ModelProviderConfig, ModelProviderTimeout, ModelProviderUnavailable

CONFIG = ModelProviderConfig(True, "http://model.invalid/v1/chat/completions", "local", 30000, 32768, 32768, 2048)
REQUEST = {"profile": {"name": "Běla", "backstory": "", "personality": ""},
           "appearance": {"race": 1, "gender": 1, "class_id": 1, "skin": 0, "face": 0,
                          "hair_style": 0, "hair_color": 0, "facial_hair": 0, "equipment": []},
           "authoring": {"role": "Sousedka", "values": "Poctivost"}, "instruction": "Připrav první návrh."}
PROPOSAL = {"backstory": "Jsem Běla. Vyrostla jsem ve vesnici a kdysi přiznala svou chybu.",
            "personality": "Mluvím přirozeně česky v ženském rodě. Když nevím, přiznám to.", "suggestions": ["Kdo zná její minulost?"]}


class Provider:
    def __init__(self):
        self.value = PROPOSAL
        self.calls = []

    async def complete(self, prompt, context):
        self.calls.append((prompt, json.loads(context)))
        if isinstance(self.value, Exception):
            raise self.value
        return self.value if isinstance(self.value, str) else json.dumps(self.value, ensure_ascii=False)


class CharacterAuthoringTests(unittest.TestCase):
    def setUp(self):
        self.provider = Provider()
        app.dependency_overrides[get_authoring_config] = lambda: CONFIG
        app.dependency_overrides[get_authoring_provider] = lambda: self.provider
        self.client = TestClient(app)

    def tearDown(self):
        self.client.close()
        app.dependency_overrides.clear()

    def post(self, body=None):
        return self.client.post("/character-studio/generate", json=REQUEST if body is None else body)

    def test_returns_strict_proposal_and_preserves_author_context(self):
        response = self.post()
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json(), PROPOSAL)
        prompt, context = self.provider.calls[0]
        self.assertIn("Czech", prompt)
        self.assertIn("0 male, 1 female", prompt)
        self.assertIn("not prescribe a fixed activity priority", prompt)
        self.assertEqual(context["profile"], REQUEST["profile"])
        self.assertNotIn("agent_id", context)

    def test_disabled_model_never_calls_provider(self):
        from dataclasses import replace
        app.dependency_overrides[get_authoring_config] = lambda: replace(CONFIG, enabled=False)
        self.assertEqual(self.post().status_code, 503)
        self.assertEqual(self.provider.calls, [])

    def test_invalid_requests_rejected_before_model(self):
        for change in ({"profile": {**REQUEST["profile"], "name": "x|cff"}},
                       {"instruction": "x\x00y"}, {"exec": "restart"},
                       {"appearance": {**REQUEST["appearance"], "gender": 2}},
                       {"appearance": {**REQUEST["appearance"], "equipment": [{"item_id": 1}]}}):
            response = self.post({**REQUEST, **change})
            self.assertEqual(response.status_code, 422, response.text)
        self.assertEqual(self.provider.calls, [])

    def test_duplicate_key_and_oversize_rejected_before_model(self):
        self.assertEqual(self.client.post("/character-studio/generate", content='{"profile":{},"profile":{}}').status_code, 422)
        self.assertEqual(self.client.post("/character-studio/generate", content=b" " * 32769).status_code, 413)
        self.assertEqual(self.provider.calls, [])

    def test_invalid_model_payload_never_becomes_fallback(self):
        cases = [{**PROPOSAL, "action": "MOVE_TO"}, {**PROPOSAL, "personality": "ž" * 1025},
                 {**PROPOSAL, "backstory": " "}, {**PROPOSAL, "suggestions": ["x|cff"]},
                 {**PROPOSAL, "suggestions": ["x"] * 5}, "```json\n{}\n```", '{"backstory":"a","backstory":"b"}']
        for value in cases:
            self.provider.value = value
            self.assertEqual(self.post().status_code, 502)

    def test_provider_errors_are_sanitized_without_retries(self):
        for value, status in ((ModelProviderTimeout("private-url-key"), 504), (ModelProviderUnavailable("private-url-key"), 502)):
            self.provider.value = value
            response = self.post()
            self.assertEqual(response.status_code, status)
            self.assertNotIn("private-url-key", response.text)
        self.assertEqual(len(self.provider.calls), 2)

    def test_separate_authoring_capacity_rejects_second_call(self):
        from unittest.mock import patch
        with patch.object(authoring_slot, "locked", return_value=True):
            self.assertEqual(self.post().status_code, 429)
        self.assertEqual(self.provider.calls, [])

    def test_crlf_authored_lines_normalize(self):
        self.provider.value = {**PROPOSAL, "backstory": "První\r\nDruhý"}
        self.assertEqual(self.post().json()["backstory"], "První\nDruhý")


if __name__ == "__main__":
    unittest.main()
