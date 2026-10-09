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

test("lab map uses its configured rectangle and preserves escaped agent diagnostics", () => {
  const scope = { map_id: 725, bounds: { min_x: 166.667, max_x: 366.667, min_y: 700, max_y: 900 } };
  const bounds = model.viewport(scope);
  const center = model.project({ x: 266.667, y: 800 }, bounds, 800, 600);
  assert.deepEqual(center, { x: 400, y: 300 });
  const northWest = model.project({ x: 366.667, y: 900 }, bounds, 800, 600);
  const southEast = model.project({ x: 166.667, y: 700 }, bounds, 800, 600);
  assert(northWest.x > 0 && northWest.x < center.x && northWest.y > 0 && northWest.y < center.y);
  assert(southEast.x > center.x && southEast.x < 800 && southEast.y > center.y && southEast.y < 600);
  const escaped = { ...base, position: { source: "live", map_id: 725, x: 400, y: 800 } };
  assert(model.matches(escaped, {}, scope));
  assert(!model.matches(base, {}, scope));
  assert(!model.matches(escaped, {}));
});

test("legacy Elwynn bounds remain the default viewport", () => {
  assert.deepEqual(model.viewport(), { north: -8100, south: -10250, west: 900, east: -1750 });
});
