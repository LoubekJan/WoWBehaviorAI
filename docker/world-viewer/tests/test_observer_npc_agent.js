"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const model = require("../app/static/observer-model.js");
const base = { position: { source: "live", map_id: 725 }, agent_id: 901035, spawn_id: 901035, name: "Worker" };
const npc = { name: "Matěj", status: "THINKING", active_action: "MOVE_TO", last_action_result: "ARRIVED",
  request_id: "18446744073709551615", next_think_ms: 2000, dialogue_count: 12, last_speech: "Mám hlad." };

test("NPC cognition facts preserve exact IDs and show only public runtime fields", () => {
  const current = { ...base, llm_agent: { ...npc, backstory: "private", model_api_key: "private" } };
  assert(model.npcAgentSupported(current));
  const facts = model.npcAgentFacts(current);
  assert(facts.some(([label, value]) => label === "Stav rozhodování" && value === "THINKING"));
  assert(facts.some(([label, value]) => label === "Požadavek" && value === "18446744073709551615"));
  assert(facts.some(([label, value]) => label === "Zapamatované repliky" && value === "12 / 12"));
  assert(facts.some(([label, value]) => label === "Vzpomínky" && value === "0 / 96"));
  assert(facts.some(([label, value]) => label === "Vybavené vzpomínky" && value === "0 / 6"));
  assert(!JSON.stringify(facts).includes("private"));
  assert(model.matches(current, { query: "matěj" }, { map_id: 725 }));
});

test("NPC memory facts show bounded counts without private memory payloads", () => {
  const current = { ...base, llm_agent: { ...npc, memory_records: 96, memory_recalled: 6,
    memories: [{ speaker: "Private player", text: "Private quote" }], relationships: ["Private player"] } };
  const facts = model.npcAgentFacts(current);
  assert(facts.some(([label, value]) => label === "Vzpomínky" && value === "96 / 96"));
  assert(facts.some(([label, value]) => label === "Vybavené vzpomínky" && value === "6 / 6"));
  assert(!JSON.stringify(facts).includes("Private"));
});

test("native background and legacy rows do not claim current LLM activity", () => {
  for (const row of [base, { ...base, llm_agent: null }, { ...base, llm_agent: npc, native_creature: true },
    { ...base, llm_agent: npc, position: { source: "spawn", map_id: 725 } }]) {
    assert(!model.npcAgentSupported(row));
    assert.deepEqual(model.npcAgentFacts(row), []);
  }
});

test("an idle NPC has no invented pending model request", () => {
  const facts = model.npcAgentFacts({ ...base, llm_agent: { ...npc, request_id: "0", dialogue_count: 0, last_speech: "" } });
  assert(facts.some(([label, value]) => label === "Požadavek" && value === null));
  assert(facts.some(([label, value]) => label === "Zapamatované repliky" && value === "0 / 12"));
});
