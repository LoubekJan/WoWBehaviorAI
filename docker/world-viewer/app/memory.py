"""Bounded demand/cache for read-only pages captured by the world thread.

All calls run on FastAPI's event loop. No database access or gameplay commands.
"""
from collections import OrderedDict
from uuid import uuid4

from fastapi import HTTPException

from .telemetry import MEMORY_PAGE_SIZE, MemoryPage

MAX_PENDING = 32
MAX_CACHED = 64
REQUEST_TTL = 60
CACHE_TTL = 30


class MemoryPages:
    def __init__(self):
        self.pending = OrderedDict()
        self.cached = OrderedDict()

    def prune(self, now, agent_ids):
        for key, (_, created) in list(self.pending.items()):
            if key[0] not in agent_ids or now - created >= REQUEST_TTL:
                del self.pending[key]
        for key, (_, received, _) in list(self.cached.items()):
            if key[0] not in agent_ids or now - received >= CACHE_TTL:
                del self.cached[key]

    def read(self, agent_id, offset, anchor, refresh, now):
        key = (agent_id, offset, anchor)
        if refresh:
            self.cached.pop(key, None)
        if key in self.cached:
            page, received, captured = self.cached[key]
            self.cached.move_to_end(key)
            return {"status": "ready", **page.model_dump(exclude={"request_id"}),
                    "captured_at_ms": captured, "age_ms": max(0, int((now - received) * 1000)),
                    "page_size": MEMORY_PAGE_SIZE}
        if key not in self.pending:
            if len(self.pending) >= MAX_PENDING:
                raise HTTPException(status_code=429, detail="Memory request queue is full; try again shortly")
            self.pending[key] = (uuid4().hex, now)
        return {"status": "pending", "page_size": MEMORY_PAGE_SIZE}

    def accept(self, page: MemoryPage | None, now, captured):
        if page is None:
            return
        key = (page.agent_id, page.offset, page.requested_anchor)
        requested = self.pending.get(key)
        if not requested or requested[0] != page.request_id:
            return  # Ignore unsolicited/late pages, including after viewer restart.
        del self.pending[key]
        self.cached[key] = (page, now, captured)
        self.cached.move_to_end(key)
        while len(self.cached) > MAX_CACHED:
            self.cached.popitem(last=False)

    def next_header(self):
        if not self.pending:
            return None
        key = next(iter(self.pending))
        request_id, _ = self.pending[key]
        self.pending.move_to_end(key)  # Round robin: a missing response cannot starve other readers.
        agent_id, offset, anchor = key
        return f"{request_id}:{agent_id}:{offset}:{anchor}"
