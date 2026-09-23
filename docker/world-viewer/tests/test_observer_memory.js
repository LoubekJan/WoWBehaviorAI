"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { ObserverMemory: memory, ObserverMemoryPanel } = require("../app/static/observer-memory.js");

test("memory pagination stays pinned while newer memories arrive", () => {
  assert.deepEqual(memory.bounds({ offset: 25, records: [{}], anchor: 26, total: 30 }),
    { previous: true, next: false, summary: "26–26 z 26" });
  assert.equal(memory.bounds({ offset: 0, records: [], anchor: 0 }).summary, "Žádné dlouhodobé vzpomínky.");
});
test("historical entities retain exact string IDs and readable events", () => {
  assert.match(memory.entity({ kind: "CREATURE", name: "Wolf", agent_id: "18446744073709551615", spawn_id: "10", entry: 30 }), /18446744073709551615/);
  assert.equal(memory.entity({ kind: "PLAYER" }), "Hráč");
  assert.equal(memory.label("CREATURE_KILLED"), "Zabití bytosti");
  assert.equal(memory.label("NEW_EVENT"), "NEW_EVENT");
  assert.equal(memory.date(0), "—");
});
test("a late fetch cannot replace memory of a newly selected NPC", async () => {
  const elements = new Map();
  const originalDocument = global.document, originalFetch = global.fetch;
  global.document = { getElementById(id) {
    if (!elements.has(id)) elements.set(id, { addEventListener() {}, replaceChildren() {}, disabled: false, textContent: "" });
    return elements.get(id);
  } };
  const requests = [];
  global.fetch = (url) => new Promise(resolve => requests.push({ url, resolve }));
  try {
    const panel = new ObserverMemoryPanel();
    const rendered = [];
    panel.render = page => rendered.push(page.agent_id);
    panel.select({ agent_id: 1 }, 3);
    panel.select({ agent_id: 2 }, 3);
    requests[1].resolve({ ok: true, json: async () => ({ status: "ready", agent_id: 2 }) });
    await new Promise(resolve => setImmediate(resolve));
    requests[0].resolve({ ok: true, json: async () => ({ status: "ready", agent_id: 1 }) });
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(rendered, [2]);
    panel.select(null, 3);
  } finally {
    global.document = originalDocument; global.fetch = originalFetch;
  }
});
