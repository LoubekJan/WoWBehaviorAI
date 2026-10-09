"""Receiver tests use FastAPI TestClient; no worldserver or network is needed."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from app.main import create_app
from app.telemetry import MAX_AGENTS, MAX_REQUEST_BYTES


def agent(agent_id: int = 80542, source: str = "live") -> dict:
    return {
        "agent_id": agent_id,
        "spawn_id": agent_id,
        "entry": 252,
        "name": "Billy Maclure",
        "type": "CIVILIAN",
        "control_mode": "AI_WORLD_CONTROLLED",
        "world_faction": 1,
        "world_state": "MATERIALIZED" if source == "live" else "ABSTRACT",
        "simulation_tier": "NEARBY",
        "position": {"x": -9923.68, "y": 38.39, "z": 32.6, "map_id": 0, "source": source},
        "health": 84,
        "max_health": 100,
        "alive": True,
        "in_combat": False,
        "needs": {
            "health_pressure": 0.16,
            "hunger": 0.71,
            "fatigue": 0.32,
            "safety_pressure": 0.0,
            "resource_pressure": 0.18,
        },
        "goal": "GET_FOOD",
        "goal_utility": 0.71,
        "action": "MOVE_TO",
        "routine_goal": "GO_HOME",
        "group_id": 17,
    }


def batch(agents: list[dict] | None = None) -> dict:
    return {"version": 1, "captured_at_ms": 1_800_000_000_000, "agents": agents if agents is not None else [agent()]}


def v2_batch() -> dict:
    # Captured from the production C++ SerializeAgentTelemetry codec.
    return json.loads((Path(__file__).parent / "fixtures" / "telemetry_v2.json").read_text(encoding="utf-8"))


def navigation_diagnostics() -> dict:
    return {'mesh': True, 'start_tile': True, 'end_tile': True,
            'filter': 3, 'start_flags': 2, 'end_flags': 1,
            'start_distance': 9.25, 'end_distance': None,
            'swimming': False, 'rejoin': False, 'failure': 'NO_COMPLETE_PATH',
            'source_x': None, 'source_y': None, 'projection_x': None, 'projection_y': None,
            'projection_z': None, 'projection_ground_z': None, 'rejected_ground_z': None,
            'previous_ground_z': None, 'connector_samples': 0, 'projection_failure': 'NONE', 'projection_probes': 0}


def return_recovery() -> dict:
    return {"failures": 8, "trail_points": 64, "retry_ms": 15000, "stalled_ms": 90000,
            "strategy": "TRAIL", "failure": "RETURN_NO_PATH", "candidates": 8, "path_type": 8,
            "requested_z": 45.57, "resolved_z": None,
            "rejected": {"invalid": 0, "height": 0, "zone": 0, "los": 0, "path": 8, "bounds": 0, "danger": 0}}


class WorldViewerApiTests(unittest.TestCase):
    def test_admission_evidence_roundtrips_without_becoming_model_requests(self):
        payload = v2_batch()
        payload['version'] = 4
        admission = dict(ready_observed=3, entries=2, acquire_attempts=1, acquired=1,
                         early_gates=1, local_empty_options=1, last_event='NO_VALID_OPTIONS',
                         last_reason='NO_VALID_OPTIONS', last_event_age_ms=5, last_acquired_age_ms=40)
        payload['agents'][0]['living_role']['advice'] = dict(
            lifetime_ms=123, enabled=True, pending=False, status='NO_VALID_OPTIONS',
            requests=0, selected=0, started=0, arrived=0, home_success=0,
            food_success=0, rejected=0, unavailable=0, reused=0, admission=admission)
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        admission['entries'] = 100
        current = self.client.get('/api/state').json()['agents'][0]['living_role']['advice']
        self.assertEqual(current['admission']['entries'], 2)
        self.assertEqual(current['admission']['local_empty_options'], 1)
        self.assertEqual(current['requests'], 0)
        self.assertFalse(current['pending'])
        self.assertEqual(current['status'], 'NO_VALID_OPTIONS')

    def test_advice_counters_roundtrip_without_changing_behavior_status(self):
        payload = v2_batch()
        payload['version'] = 4
        advice = dict(lifetime_ms=123, enabled=True, pending=False, status='MOVE_STARTED',
                      requests=3, selected=2, started=1, arrived=0, home_success=0,
                      food_success=0, rejected=1, unavailable=0, reused=0,
                      empty_search_rounds=2, failed_food_advice=3, known_food_places=4,
                      retry_ms=900000, search_radius=128.0, queue_wait_ms=45000, queue_size=8,
                      queue_dispatchable=True, queue_kind='RETURN')
        payload['agents'][0]['living_role']['advice'] = advice
        payload['agents'][0]['living_role'].update(move_end='NO_PROGRESS', move_no_progress_ms=15000, move_remaining=42.5)
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        role = self.client.get('/api/state').json()['agents'][0]['living_role']
        self.assertEqual(role['advice'], {**advice, 'admission': None})
        self.assertEqual(role['phase'], payload['agents'][0]['living_role']['phase'])
        self.assertEqual(role['move_end'], 'NO_PROGRESS')
        self.assertEqual(role['move_remaining'], 42.5)
        for field, value in [('requests', -1), ('started', True), ('status', 'x' * 81), ('unknown', 1),
                             ('search_radius', 129.0), ('empty_search_rounds', 3), ('failed_food_advice', 4),
                             ('queue_wait_ms', -1), ('queue_size', 2049), ('queue_dispatchable', 1), ('queue_kind', 'UNKNOWN')]:
            with self.subTest(field=field):
                bad = copy.deepcopy(payload)
                bad['agents'][0]['living_role']['advice'][field] = value
                self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['advice'], {**advice, 'admission': None})

    def test_recovery_protocol_roundtrips_and_rejects_bad_diagnostics(self):
        payload = v2_batch()
        payload['version'] = 4
        recovery = return_recovery()
        payload['agents'][0]['living_role']['return_recovery'] = recovery
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        state = self.client.get('/api/state').json()
        self.assertEqual(state['version'], 4)
        self.assertEqual(state['agents'][0]['living_role']['return_recovery'],
                         {**recovery, 'navigation': None, 'backtracks': 0, 'rejoins': 0, 'corridor_points': 0,
                          'home_path_type': 0, 'home_path_failure': 'NOT_CHECKED',
                          'home_surface_failure': 'NOT_CHECKED', 'home_path_surface': False,
                          'home_detour_failure': 'NOT_CHECKED', 'home_detour_nodes': 0, 'home_detour_edges': 0,
                          'home_path_rejected': dict.fromkeys(('ground', 'path', 'endpoint', 'bounds', 'danger', 'corridor'), 0),
                          'continuation_path_type': 0, 'continuation_path_failure': 'NOT_CHECKED',
                          'continuation_surface_failure': 'NOT_CHECKED', 'continuation_path_surface': False,
                          'continuation_detour_failure': 'NOT_CHECKED', 'continuation_detour_nodes': 0, 'continuation_detour_edges': 0,
                          'planning_deferred': False, 'surface_corridor': False,
                          'continuation_path_rejected': dict.fromkeys(('ground', 'path', 'endpoint', 'bounds', 'danger', 'corridor'), 0),
                          'refuge_active': False, 'refuge_episodes': 0, 'refuge_moves': 0,
                          'refuge_blocked': 0, 'refuge_remaining_ms': 0, 'refuge_anchor': None})
        recovery['rejected']['path'] = -1
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 422)
        # A malformed batch must not replace the last good observation.
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery']['rejected']['path'], 8)

    def test_navigation_diagnostics_roundtrip_and_limits(self):
        payload = v2_batch()
        payload['version'] = 4
        nav = navigation_diagnostics()
        nav.update(detail="HOME_RADIUS", home_radius=96.0, rejected_x=10.0, rejected_y=20.0, rejected_z=30.0,
                   source_z=53.25, support_z=55.5)
        nav.update(projection_x=10.25, projection_y=20.0, projection_z=53.5, projection_ground_z=55.5,
                   rejected_ground_z=57.0, previous_ground_z=55.5, connector_samples=4)
        recovery = {**return_recovery(), 'navigation': nav, 'backtracks': 2, 'rejoins': 1, 'corridor_points': 3,
                    'home_path_type': 4, 'home_path_failure': 'NO_COMPLETE_PATH',
                    'home_surface_failure': 'NOT_CHECKED', 'home_path_surface': False,
                    'home_detour_failure': 'NOT_CHECKED', 'home_detour_nodes': 0, 'home_detour_edges': 0,
                    'home_path_rejected': dict(ground=7, path=2, endpoint=0, bounds=0, danger=0, corridor=0),
                    'continuation_path_type': 1, 'continuation_path_failure': 'ENDPOINT_MISMATCH',
                    'continuation_surface_failure': 'NOT_CHECKED', 'continuation_path_surface': False,
                    'continuation_detour_failure': 'NOT_CHECKED', 'continuation_detour_nodes': 0, 'continuation_detour_edges': 0,
                    'planning_deferred': False, 'surface_corridor': False,
                    'continuation_path_rejected': dict(ground=0, path=8, endpoint=1, bounds=0, danger=0, corridor=0),
                    'refuge_active': True, 'refuge_episodes': 2, 'refuge_moves': 3,
                    'refuge_blocked': 4, 'refuge_remaining_ms': 120000,
                    'refuge_anchor': {'map_id': 0, 'x': 10.0, 'y': 20.0, 'z': 30.0}}
        payload['agents'][0]['living_role']['return_recovery'] = recovery
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery'], recovery)
        for field, value in [('home_path_type', -1), ('home_path_type', True), ('home_path_type', 256),
                             ('home_path_failure', 'x' * 101), ('refuge_active', 1),
                             ('home_path_rejected', {'path': -1}), ('home_path_rejected', {'ground': True}),
                             ('home_path_rejected', {'unknown': 1}), ('continuation_path_rejected', {'path': 10}),
                             ('continuation_path_type', 256), ('continuation_path_failure', 'x' * 101),
                             ('refuge_episodes', True), ('refuge_moves', -1), ('refuge_remaining_ms', 120001)]:
            bad = copy.deepcopy(payload)
            bad['agents'][0]['living_role']['return_recovery'][field] = value
            self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        for field, value in [('start_distance', -1), ('end_distance', 'NaN'), ('filter', 65536),
                             ('failure', 'x' * 81), ('detail', 'x' * 81), ('home_radius', -1), ('unknown', 1),
                             ('source_z', 'NaN'), ('support_z', True), ('projection_ground_z', 'NaN'),
                             ('rejected_ground_z', True), ('connector_samples', -1), ('connector_samples', 65),
                             ('projection_probes', 65), ('projection_failure', 'x' * 81)]:
            with self.subTest(field=field):
                bad = copy.deepcopy(payload)
                bad['agents'][0]['living_role']['return_recovery']['navigation'][field] = value
                self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        recovery['backtracks'] = 17
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 422)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery']['backtracks'], 2)

    def test_planning_without_return_recovery_roundtrips_and_invalid_batches_do_not_replace_it(self):
        payload = v2_batch()
        payload['version'] = 4
        role = payload['agents'][0]['living_role']
        role.pop('return_recovery', None)
        role['movement_purpose'] = 'PLANNING_DEFERRED'
        planning = dict(deferred=True, reason='ADMISSION', stage='DECISION', wait_ms=67000,
                        query_age_ms=68000, no_progress_ms=67000, resets=2)
        role['planning'] = planning
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        actual = self.client.get('/api/state').json()['agents'][0]['living_role']
        self.assertEqual(actual['planning'], planning)
        self.assertIsNone(actual['return_recovery'])
        for field, value in [('deferred', 1), ('reason', 'UNKNOWN'), ('stage', 'UNKNOWN'), ('wait_ms', -1),
                             ('query_age_ms', True), ('no_progress_ms', '1'), ('resets', 4294967296), ('unknown', 1)]:
            with self.subTest(field=field):
                bad = copy.deepcopy(payload)
                bad['agents'][0]['living_role']['planning'][field] = value
                self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        bad = copy.deepcopy(payload)
        bad['agents'][0]['living_role']['planning'] = None
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['planning'], planning)

    def test_planning_and_surface_diagnostics_are_independent_strict_fields(self):
        payload = v2_batch()
        payload['version'] = 4
        recovery = {**return_recovery(), 'planning_deferred': True, 'surface_corridor': True,
                    'home_surface_failure': 'SURFACE_OBSTACLE', 'home_path_surface': False,
                    'continuation_surface_failure': 'NONE', 'continuation_path_surface': True}
        payload['agents'][0]['living_role']['return_recovery'] = recovery
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        actual = self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery']
        for field in ('planning_deferred', 'surface_corridor', 'home_surface_failure', 'home_path_surface',
                      'continuation_surface_failure', 'continuation_path_surface'):
            self.assertEqual(actual[field], recovery[field])
        self.assertEqual(actual['failure'], 'RETURN_NO_PATH')
        self.assertEqual(actual['home_path_failure'], 'NOT_CHECKED')
        self.assertEqual(actual['rejected']['path'], 8)
        for field in ('planning_deferred', 'surface_corridor', 'home_path_surface', 'continuation_path_surface'):
            for value in (0, 1, 'true', 'false', None):
                with self.subTest(field=field, value=value):
                    bad = copy.deepcopy(payload)
                    bad['agents'][0]['living_role']['return_recovery'][field] = value
                    self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        for field in ('home_surface_failure', 'continuation_surface_failure'):
            for value in (1, True, None, 'x' * 101):
                with self.subTest(field=field, value=value):
                    bad = copy.deepcopy(payload)
                    bad['agents'][0]['living_role']['return_recovery'][field] = value
                    self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        bad = copy.deepcopy(payload)
        bad['agents'][0]['living_role']['return_recovery']['unknown_planning_flag'] = True
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery'], actual)

    def test_detour_diagnostics_roundtrip_and_invalid_batch_keeps_previous_state(self):
        payload = v2_batch()
        payload['version'] = 4
        recovery = {**return_recovery(), 'home_detour_failure': 'DETOUR_PENDING',
                    'home_detour_nodes': 24, 'home_detour_edges': 37,
                    'continuation_detour_failure': 'NONE', 'continuation_detour_nodes': 8,
                    'continuation_detour_edges': 12}
        payload['agents'][0]['living_role']['return_recovery'] = recovery
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        actual = self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery']
        for field, value in recovery.items():
            self.assertEqual(actual[field], value)
        self.assertEqual(actual['home_path_failure'], 'NOT_CHECKED')
        self.assertFalse(actual['surface_corridor'])
        for prefix in ('home', 'continuation'):
            invalid = [(f'{prefix}_detour_failure', value) for value in (1, True, None, 'x' * 101)]
            for suffix in ('nodes', 'edges'):
                invalid += [(f'{prefix}_detour_{suffix}', value) for value in (-1, True, '1', 1.5, None, 4294967296)]
            for field, value in invalid:
                with self.subTest(field=field, value=value):
                    bad = copy.deepcopy(payload)
                    bad['agents'][0]['living_role']['return_recovery'][field] = value
                    self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery'], actual)

    def test_forage_diagnostics_roundtrip_and_validation(self):
        payload = v2_batch()
        payload['version'] = 4
        from app.telemetry import NavigationDiagnostics
        nav = NavigationDiagnostics(**navigation_diagnostics()).model_dump()
        forage = dict(scanned_at_ms=900, search_at_ms=1000, nearby_prey=2, attackable_prey=1,
                      reachable_prey=0, route_attempts=8, height_rejected=2, path_rejected=6, steps_started=0,
                      navigation=nav)
        payload['agents'][0]['living_role']['forage'] = forage
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['forage'], forage)
        for key, value in [('scanned_at_ms', -1), ('route_attempts', True), ('reachable_prey', -1), ('unknown', 1)]:
            bad = copy.deepcopy(payload)
            bad['agents'][0]['living_role']['forage'][key] = value
            self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=bad).status_code, 422)

    def setUp(self) -> None:
        self.client = TestClient(create_app("test-secret"))
        self.headers = {"Authorization": "Bearer test-secret"}

    def tearDown(self) -> None:
        self.client.close()

    def test_public_state_starts_empty_and_stale(self) -> None:
        state = self.client.get("/api/state").json()
        self.assertTrue(state["configured"])
        self.assertTrue(state["stale"])
        self.assertIsNone(state["captured_at_ms"])
        self.assertEqual(state["agents"], [])
        self.assertEqual(state["scope"], {"map_id": 0, "zone_ids": [12], "bounds": None, "name": "Elwynn Forest"})

    def test_lab_scope_is_api_configuration_and_escaped_points_are_preserved(self) -> None:
        from test_scope import LAB_SCOPE
        env = {"WORLD_VIEWER_SCOPE_MAP_ID": "725", "WORLD_VIEWER_SCOPE_ZONE_IDS": "4988",
               "WORLD_VIEWER_SCOPE_NAME": "AI World Lab", "WORLD_VIEWER_SCOPE_BOUNDS": json.dumps(LAB_SCOPE['bounds'])}
        with patch.dict('os.environ', env), TestClient(create_app("test-secret")) as client:
            npc = agent()
            npc['position'].update(map_id=725, x=400, y=800, z=0)
            payload = batch([npc])
            self.assertEqual(client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
            state = client.get('/api/state').json()
            self.assertEqual(state['scope'], LAB_SCOPE)
            self.assertEqual(state['agents'][0]['position'], npc['position'])
            payload['scope'] = LAB_SCOPE
            self.assertEqual(client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 422)

    def test_post_requires_correct_bearer_token(self) -> None:
        for headers in ({}, {"Authorization": "Basic test-secret"}, {"Authorization": "Bearer wrong"}):
            with self.subTest(headers=headers):
                self.assertEqual(self.client.post("/internal/telemetry", headers=headers, json=batch()).status_code, 401)
        self.assertEqual(self.client.get("/api/state").json()["agents"], [])

    def test_unconfigured_service_rejects_ingest(self) -> None:
        with TestClient(create_app("")) as client:
            self.assertFalse(client.get("/health").json()["telemetry_configured"])
            self.assertEqual(client.post("/internal/telemetry", headers=self.headers, json=batch()).status_code, 503)

    def test_accepts_snapshot_and_replaces_previous_batch(self) -> None:
        response = self.client.post("/internal/telemetry", headers=self.headers, json=batch([agent(), agent(80543, "spawn")]))
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json(), {"accepted": 2})
        state = self.client.get("/api/state").json()
        self.assertEqual(state["captured_at_ms"], 1_800_000_000_000)
        self.assertFalse(state["stale"])
        self.assertEqual([item["agent_id"] for item in state["agents"]], [80542, 80543])
        self.assertEqual(state["agents"][1]["position"]["source"], "spawn")
        self.assertEqual(self.client.post("/internal/telemetry", headers=self.headers, json=batch([agent(9)])).json(), {"accepted": 1})
        self.assertEqual([item["agent_id"] for item in self.client.get("/api/state").json()["agents"]], [9])

    def test_stale_uses_receive_clock_not_source_timestamp(self) -> None:
        self.client.post("/internal/telemetry", headers=self.headers, json=batch())
        app = self.client.app
        with patch("app.main.time.monotonic", return_value=app.state.received_monotonic + 5.1):
            state = self.client.get("/api/state").json()
        self.assertTrue(state["stale"])
        self.assertGreaterEqual(state["age_ms"], 5000)

    def test_cpp_v2_snapshot_preserves_diagnostics_and_background_semantics(self) -> None:
        payload = v2_batch()
        response = self.client.post("/internal/telemetry", headers=self.headers, json=payload)
        self.assertEqual(response.status_code, 200, response.text)
        state = self.client.get("/api/state").json()
        self.assertEqual(state["version"], 2)
        # v2 omitted this later optional field; all original values survive.
        expected = copy.deepcopy(payload["agents"])
        expected[0]["living_role"]["return_recovery"] = None
        expected[0]["living_role"]["advice"] = None
        expected[0]["living_role"]["forage"] = None
        expected[0]["living_role"].update(move_end='NONE', move_no_progress_ms=0, move_remaining=None)
        expected[0]["living_role"]["planning"] = dict(deferred=False, reason='NONE', stage='NONE', wait_ms=0,
                                                    query_age_ms=0, no_progress_ms=0, resets=0)
        self.assertEqual(state["agents"], expected)
        live, background = state["agents"]
        self.assertEqual(live["economy"]["money"], "18446744073709551615")
        self.assertEqual(live["living_role"]["sprint_remaining_ms"], 450)
        self.assertEqual([group["id"] for group in live["groups"]], [17, 18])
        self.assertEqual(live["reputation_faction_id"], 1204)
        self.assertIsNone(background["living_role"])
        self.assertIsNone(background["faction_template_id"])
        self.assertEqual(background["economy"]["food"], 4)

    def test_v1_can_replace_v2_without_retaining_new_fields(self) -> None:
        self.client.post("/internal/telemetry", headers=self.headers, json=v2_batch())
        self.client.post("/internal/telemetry", headers=self.headers, json=batch())
        state = self.client.get("/api/state").json()
        self.assertEqual(state["version"], 1)
        self.assertIsNone(state["agents"][0]["living_role"])
        self.assertEqual(state["agents"][0]["groups"], [])

    def test_rejects_live_diagnostics_on_background_agent(self) -> None:
        payload = v2_batch()
        self.client.post("/internal/telemetry", headers=self.headers, json=payload)
        accepted_agents = self.client.get("/api/state").json()["agents"]
        for key in ("living_role", "movement", "target", "destination", "faction_template_id"):
            invalid = copy.deepcopy(payload)
            invalid["agents"][1][key] = invalid["agents"][0][key]
            with self.subTest(field=key):
                response = self.client.post("/internal/telemetry", headers=self.headers, json=invalid)
                self.assertEqual(response.status_code, 422, response.text)
        self.assertEqual(self.client.get("/api/state").json()["agents"], accepted_agents)

    def test_full_elwynn_snapshot_fits_and_is_accepted(self) -> None:
        payload = v2_batch()
        payload["version"] = 4
        prototype = payload["agents"][0]
        prototype["living_role"]["return_recovery"] = return_recovery()
        prototype["living_role"]["return_recovery"].update(navigation=navigation_diagnostics(), backtracks=16)
        payload["agents"] = [{**prototype, "agent_id": i, "spawn_id": i} for i in range(3540)]
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.assertLess(len(body), MAX_REQUEST_BYTES)
        response = self.client.post("/internal/telemetry", headers=self.headers, content=body)
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json(), {"accepted": 3540})

    def test_rejects_invalid_or_duplicate_batches_without_replacing_cache(self) -> None:
        self.client.post("/internal/telemetry", headers=self.headers, json=batch())
        invalid = [
            {**batch(), "version": 99},
            batch([{**agent(), "position": {**agent()["position"], "source": "unknown"}}]),
            batch([{**agent(), "needs": {**agent()["needs"], "hunger": 1.5}}]),
            batch([agent(1), agent(1)]),
        ]
        for payload in invalid:
            with self.subTest(payload=payload.get("version")):
                self.assertEqual(self.client.post("/internal/telemetry", headers=self.headers, json=payload).status_code, 422)
        self.assertEqual(self.client.get("/api/state").json()["agents"][0]["agent_id"], 80542)

    def test_rejects_malformed_json_without_replacing_cache(self) -> None:
        self.client.post("/internal/telemetry", headers=self.headers, json=batch())
        response = self.client.post("/internal/telemetry", headers=self.headers, content=b"{broken")
        self.assertEqual(response.status_code, 400)
        self.assertEqual(self.client.get("/api/state").json()["agents"][0]["agent_id"], 80542)

    def test_payload_byte_cap_and_agent_cap(self) -> None:
        oversized = b"{" + b" " * MAX_REQUEST_BYTES + b"}"
        response = self.client.post("/internal/telemetry", headers=self.headers, content=oversized)
        self.assertEqual(response.status_code, 413)
        too_many = batch([agent(i) for i in range(MAX_AGENTS + 1)])
        self.assertEqual(self.client.post("/internal/telemetry", headers=self.headers, json=too_many).status_code, 422)

    def test_page_is_read_only_and_served_without_token(self) -> None:
        response = self.client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn("Elwynn Forest", response.text)
        self.assertEqual(self.client.post("/api/state").status_code, 405)


if __name__ == "__main__":
    unittest.main()
