"""Optional public NPC cognition metadata preserves older observer payloads."""
import copy
import json
from pathlib import Path
import unittest

from fastapi.testclient import TestClient
from pydantic import ValidationError
from app.main import create_app
from app.telemetry import Agent, NpcAgent


def metadata():
    return dict(name='Matěj', status='THINKING', last_action_result='ARRIVED',
                active_action='MOVE_TO', request_id='18446744073709551615',
                next_think_ms=2000, dialogue_count=12, last_speech='Mám hlad, zajdu do skladu.')


def payload():
    path = Path(__file__).parent / 'fixtures' / 'telemetry_v2.json'
    batch = json.loads(path.read_text(encoding='utf-8'))
    batch['version'] = 4
    batch['agents'][0]['control_mode'] = 'AI_WORLD_CONTROLLED'
    return batch


class NpcAgentTelemetryTests(unittest.TestCase):
    def test_legacy_payloads_default_to_unavailable_cognition(self):
        for name in ('telemetry_v2.json', 'telemetry_v3_memory.json', 'telemetry_v4_native.json'):
            data = json.loads((Path(__file__).parent / 'fixtures' / name).read_text(encoding='utf-8'))
            for row in data['agents']:
                with self.subTest(name=name, agent=row['agent_id']):
                    self.assertIsNone(Agent.model_validate(row).llm_agent)

    def test_owned_public_fields_roundtrip_without_authored_lore(self):
        source = metadata()
        parsed = NpcAgent.model_validate(source)
        source['last_speech'] = 'Changed elsewhere'
        self.assertEqual(parsed.model_dump(), metadata())
        self.assertEqual(NpcAgent.model_validate_json(parsed.model_dump_json()), parsed)
        self.assertNotIn('backstory', parsed.model_dump())
        self.assertNotIn('personality', parsed.model_dump())

    def test_ids_numeric_bounds_and_fields_are_strict(self):
        for field, invalid in (
            ('request_id', 1), ('request_id', '01'), ('request_id', '-1'),
            ('request_id', '18446744073709551616'), ('next_think_ms', -1),
            ('next_think_ms', True), ('next_think_ms', '1000'),
            ('next_think_ms', 18446744073709551616), ('dialogue_count', -1),
            ('dialogue_count', 13), ('dialogue_count', True), ('dialogue_count', '1'),
            ('name', ''), ('name', '  '), ('name', 'x' * 33),
            ('status', 'x' * 81), ('last_action_result', 'x' * 129),
            ('active_action', 'x' * 33), ('last_speech', 'x' * 257),
            ('backstory', 'Private background'), ('model_api_key', 'private'),
        ):
            with self.subTest(field=field, invalid=invalid), self.assertRaises(ValidationError):
                NpcAgent.model_validate({**metadata(), field: invalid})

    def test_plain_utf8_text_rejects_controls_markup_and_byte_overflow(self):
        for field in ('name', 'status', 'last_action_result', 'active_action', 'last_speech'):
            for invalid in ('a\nb', '\x00', '\x85', '\u202e', '\ud800', '|cffffffffspoof|r'):
                with self.subTest(field=field, invalid=repr(invalid)), self.assertRaises(ValidationError):
                    NpcAgent.model_validate({**metadata(), field: invalid})
        self.assertEqual(NpcAgent.model_validate({**metadata(), 'last_speech': '中' * 256}).last_speech, '中' * 256)
        for field, invalid in (('name', '中' * 27), ('last_speech', '🍞' * 193)):
            with self.subTest(field=field), self.assertRaises(ValidationError):
                NpcAgent.model_validate({**metadata(), field: invalid})

    def test_only_live_controlled_managed_rows_accept_cognition(self):
        row = payload()['agents'][0]
        row['llm_agent'] = metadata()
        self.assertEqual(Agent.model_validate(row).llm_agent.name, 'Matěj')
        for control in ('OBSERVE_ONLY', 'UNKNOWN'):
            invalid = {**row, 'control_mode': control}
            with self.subTest(control=control), self.assertRaises(ValidationError):
                Agent.model_validate(invalid)
        invalid = copy.deepcopy(row)
        invalid['position']['source'] = 'spawn'
        with self.assertRaises(ValidationError):
            Agent.model_validate(invalid)
        invalid = copy.deepcopy(row)
        invalid.update(native_creature=True, control_mode='OBSERVE_ONLY', needs=None, economy=None, living_role=None)
        with self.assertRaises(ValidationError):
            Agent.model_validate(invalid)

    def test_viewer_preserves_every_scalar_allowed_by_the_cpp_profile_name_contract(self):
        for name in ('Ma\u00adtěj', 'Ma\u061ctěj', '\u2002'):
            with self.subTest(name=repr(name)):
                self.assertEqual(NpcAgent.model_validate({**metadata(), 'name': name}).name, name)
        for name in ('\u00a0', ' \u00a0 '):
            with self.subTest(name=repr(name)), self.assertRaises(ValidationError):
                NpcAgent.model_validate({**metadata(), 'name': name})

    def test_invalid_npc_batch_keeps_previous_snapshot(self):
        with TestClient(create_app('test-secret')) as client:
            headers = {'Authorization': 'Bearer test-secret'}
            batch = payload()
            batch['agents'][0]['llm_agent'] = metadata()
            response = client.post('/internal/telemetry', headers=headers, json=batch)
            self.assertEqual(response.status_code, 200, response.text)
            observed = client.get('/api/state').json()
            self.assertEqual(observed['agents'][0]['llm_agent'], metadata())
            self.assertEqual(len(observed['agents']), len(batch['agents']))
            invalid = copy.deepcopy(batch)
            invalid['agents'][0]['llm_agent']['dialogue_count'] = 13
            self.assertEqual(client.post('/internal/telemetry', headers=headers, json=invalid).status_code, 422)
            self.assertEqual(client.get('/api/state').json()['agents'][0]['llm_agent'], metadata())


if __name__ == '__main__':
    unittest.main()
