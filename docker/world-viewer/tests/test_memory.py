"""Demand-driven memory reads: scoped, bounded, and separate from map state."""
import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from app.main import create_app
from app.memory import MAX_PENDING
from app.telemetry import MEMORY_PAGE_SIZE, MAX_REQUEST_BYTES
from test_world_viewer import v2_batch


def memory_fixture():
    return json.loads((Path(__file__).parent / "fixtures" / "telemetry_v3_memory.json").read_text(encoding="utf-8"))


class MemoryApiTests(unittest.TestCase):
    def setUp(self):
        self.client = TestClient(create_app("test-secret"))
        self.headers = {"Authorization": "Bearer test-secret"}
        self.payload = {**v2_batch(), "version": 3}
        self.post()

    def tearDown(self):
        self.client.close()

    def post(self, payload=None):
        return self.client.post("/internal/telemetry", headers=self.headers, json=payload or self.payload)

    def requested_page(self, agent_id=80542):
        self.assertEqual(self.client.get(f"/api/agents/{agent_id}/memory").json()["status"], "pending")
        header = self.post().headers["x-observer-memory-request"]
        request_id, requested_agent, offset, anchor = header.split(":")
        self.assertEqual(int(requested_agent), agent_id)
        page = memory_fixture()["memory_page"]
        page.update(request_id=request_id, agent_id=agent_id, offset=int(offset), requested_anchor=int(anchor))
        return page

    def test_cpp_memory_round_trip_is_requested_only_after_click(self):
        self.assertNotIn("x-observer-memory-request", self.post().headers)
        page = self.requested_page()
        response = self.post({**self.payload, "memory_page": page})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertNotIn("x-observer-memory-request", response.headers)
        result = self.client.get("/api/agents/80542/memory").json()
        self.assertEqual(result["status"], "ready")
        self.assertEqual(result["records"], page["records"])
        self.assertEqual(result["records"][0]["persistent_id"], "18446744073709551615")
        state = self.client.get("/api/state").json()
        self.assertNotIn("memory_page", state)
        self.assertNotIn("records", state["agents"][0])

    def test_background_agents_can_have_memories_and_empty_is_distinct_from_pending(self):
        page = self.requested_page(80543)
        self.post({**self.payload, "memory_page": page})
        self.assertEqual(len(self.client.get("/api/agents/80543/memory").json()["records"]), 1)
        self.assertEqual(self.client.get("/api/agents/80543/memory?refresh=true").json()["status"], "pending")
        page["request_id"] = self.post().headers["x-observer-memory-request"].split(":")[0]
        page.update(total=0, anchor=0, records=[])
        self.post({**self.payload, "memory_page": page})
        empty = self.client.get("/api/agents/80543/memory").json()
        self.assertEqual(empty["status"], "ready")
        self.assertEqual(empty["records"], [])
        self.assertEqual(empty["total"], 0)

    def test_late_unmatched_page_does_not_fulfil_another_request(self):
        page = self.requested_page()
        page["request_id"] = "0" * 32
        self.post({**self.payload, "memory_page": page})
        self.assertEqual(self.client.get("/api/agents/80542/memory").json()["status"], "pending")
        self.assertFalse(self.client.app.state.memory_pages.cached)

    def test_unknown_agents_and_invalid_bounds_are_rejected(self):
        self.assertEqual(self.client.get("/api/agents/999999/memory").status_code, 404)
        for query in ("offset=-1", "anchor=-1", "offset=4294967296"):
            self.assertEqual(self.client.get(f"/api/agents/80542/memory?{query}").status_code, 422)
        self.assertNotIn("x-observer-memory-request", self.post().headers)

    def test_memory_requests_are_not_sent_to_old_or_offline_servers(self):
        with patch("app.main.time.monotonic", return_value=self.client.app.state.received_monotonic + 6):
            self.assertEqual(self.client.get("/api/agents/80542/memory").json()["status"], "offline")
        self.requested_page()
        response = self.post(v2_batch())
        self.assertNotIn("x-observer-memory-request", response.headers)
        self.assertEqual(self.client.get("/api/agents/80542/memory").json()["status"], "unsupported")
        self.assertFalse(self.client.app.state.memory_pages.pending)

    def test_queue_is_bounded_fair_and_prunes_expired_and_removed_agents(self):
        prototype = self.payload["agents"][0]
        self.payload["agents"] = [{**prototype, "agent_id": i} for i in range(1, MAX_PENDING + 2)]
        self.post()
        for i in range(1, MAX_PENDING + 1):
            self.assertEqual(self.client.get(f"/api/agents/{i}/memory").status_code, 200)
        self.assertEqual(self.client.get(f"/api/agents/{MAX_PENDING + 1}/memory").status_code, 429)
        headers = [self.post().headers["x-observer-memory-request"] for _ in range(MAX_PENDING)]
        self.assertEqual(len(set(headers)), MAX_PENDING)
        pages = self.client.app.state.memory_pages
        pages.prune(self.client.app.state.received_monotonic + 61, set(range(1, MAX_PENDING + 2)))
        self.assertFalse(pages.pending)
        pages.read(1, 0, 0, False, 0)
        pages.prune(1, set())
        self.assertFalse(pages.pending)

    def test_oversized_or_inconsistent_memory_page_is_rejected(self):
        page = self.requested_page()
        for invalid in ({**page, "anchor": 2}, {**page, "records": page["records"] * (MEMORY_PAGE_SIZE + 1)}):
            response = self.post({**self.payload, "memory_page": invalid})
            self.assertEqual(response.status_code, 422, response.text)
        self.assertEqual(len(self.client.get("/api/state").json()["agents"]), 2)

    def test_complete_memory_page_fits_alongside_full_elwynn_snapshot(self):
        page = self.requested_page()
        page.update(records=page["records"] * MEMORY_PAGE_SIZE, anchor=MEMORY_PAGE_SIZE, total=MEMORY_PAGE_SIZE)
        payload = copy.deepcopy(self.payload)
        prototype = payload["agents"][0]
        payload["agents"] = [{**prototype, "agent_id": i} for i in range(1, 3541)] + [prototype]
        payload["memory_page"] = page
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.assertLess(len(body), MAX_REQUEST_BYTES)
        response = self.client.post("/internal/telemetry", headers=self.headers, content=body)
        self.assertEqual(response.status_code, 200, response.text)
