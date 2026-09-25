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


def return_recovery() -> dict:
    return {"failures": 8, "trail_points": 64, "retry_ms": 15000, "stalled_ms": 90000,
            "strategy": "TRAIL", "failure": "RETURN_NO_PATH", "candidates": 8, "path_type": 8,
            "requested_z": 45.57, "resolved_z": None,
            "rejected": {"invalid": 0, "height": 0, "zone": 0, "los": 0, "path": 8, "bounds": 0, "danger": 0}}


class WorldViewerApiTests(unittest.TestCase):
    def test_recovery_protocol_roundtrips_and_rejects_bad_diagnostics(self):
        payload = v2_batch()
        payload['version'] = 4
        recovery = return_recovery()
        payload['agents'][0]['living_role']['return_recovery'] = recovery
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 200)
        state = self.client.get('/api/state').json()
        self.assertEqual(state['version'], 4)
        self.assertEqual(state['agents'][0]['living_role']['return_recovery'], recovery)
        recovery['rejected']['path'] = -1
        self.assertEqual(self.client.post('/internal/telemetry', headers=self.headers, json=payload).status_code, 422)
        # A malformed batch must not replace the last good observation.
        self.assertEqual(self.client.get('/api/state').json()['agents'][0]['living_role']['return_recovery']['rejected']['path'], 8)

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
