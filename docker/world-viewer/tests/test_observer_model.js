"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const model = require("../app/static/observer-model.js");
const base = { position: { source: "live", map_id: 0 }, needs: { hunger: 0.8 }, agent_id: 1, spawn_id: 10 };

test("a wolf group hunt has a visible goal and hunting phase even without individual goal", () => {
  const wolf = { ...base, living_wolf: true, coordination_goal: "HUNT", goal: null, groups: [{ id: 17 }, { id: 18 }] };
  assert.equal(model.goal(wolf), "HUNT");
  assert.equal(model.phase(wolf), "HUNTING");
  assert(model.matches(wolf, { goal: true, phase: "HUNTING", group: "18" }));
  assert(!model.matches(wolf, { group: "19" }));
});
test("last hunt status alone never claims a current hunt", () => {
  const spider = { ...base, living_role: { phase: "IDLE", hunt_status: "HUNT_STARTED", hunt_end: "PREY_ESCAPED", awareness: "QUIET" } };
  assert.equal(model.phase(spider), "IDLE");
  assert.equal(model.alert(spider), false);
  assert(!model.matches(spider, { phase: "HUNTING" }));
});
test("a dead wolf is not shown as hunting from a retained goal", () => {
  const wolf = { ...base, living_wolf: true, alive: false, effective_goal: "HUNT", in_combat: true };
  assert.equal(model.phase(wolf), "DEAD");
  assert.equal(model.alert(wolf), false);
});
test("danger and movement filters work independently", () => {
  const guard = { ...base, living_role: { phase: "INVESTIGATING", awareness: "ALARM" }, movement: { cannot_reach_target: true } };
  assert(model.matches(guard, { alert: true, blocked: true }));
  assert(!model.matches(base, { blocked: true }));
});
test("background and old telemetry stay usable without fabricated behavior", () => {
  const old = { ...base, group_id: 17, routine_goal: "GO_HOME" };
  assert.equal(model.phase(old), "UNKNOWN");
  assert(model.matches(old, { group: "17", goal: true }));
  const background = { ...old, position: { source: "spawn", map_id: 0 } };
  assert.equal(model.phase(background), "BACKGROUND");
  assert(!model.alert(background));
  assert(!model.matches(background, { live: true }));
});
