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

test("native observation phases use live flags and never synthesize AI hunger or goals", () => {
  const native = { ...base, native_creature: true, alive: true, needs: null, world_faction: 2,
    type: "COMBAT", goal: "HUNT", group_id: 17 };
  assert.equal(model.goal(native), null);
  assert.equal(model.phase(native), "IDLE");
  assert.equal(model.aiDetailsSupported(native), false);
  assert.deepEqual(model.groupIds(native), []);
  assert(!model.matches(native, { goal: true }));
  assert(!model.matches(native, { hunger: true }));
  assert(!model.matches(native, { role: "WOLF_PACK" }));
  assert(model.matches(native, { faction: "2", type: "COMBAT", live: true }));
  assert.equal(model.phase({ ...native, in_combat: true }), "COMBAT");
  assert.equal(model.phase({ ...native, movement: { moving: true } }), "MOVING");
  assert.equal(model.phase({ ...native, alive: false }), "DEAD");
  assert.equal(model.phase({ ...native, alive: null }), "UNKNOWN");
  const unloaded = { ...native, alive: null, in_combat: null, position: { source: "spawn", map_id: 0 } };
  assert.equal(model.phase(unloaded), "NOT_LOADED");
  assert(!model.alert(unloaded));
  assert(!model.matches(unloaded, { live: true }));
});

test("native details hide AI sections, retain observed state, and restore AI details on selection", () => {
  const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
  const staticDir = path.join(__dirname, "../app/static");
  class Element {
    constructor() { this.children = []; this.hidden = false; this.style = {}; this.value = ""; this._text = ""; }
    addEventListener() {}
    replaceChildren(...children) { this.children = children; this._text = ""; }
    append(...children) { this.children.push(...children); }
    get textContent() { return this._text || this.children.map(child => child.textContent || "").join(""); }
    set textContent(value) { this._text = value; this.children = []; }
    getContext() { return {}; }
    setAttribute() {}
  }
  const elements = new Map(), requests = [];
  const context = vm.createContext({
    document: { getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
      createElement() { return new Element(); }, createTextNode(text) { return { textContent: text }; } },
    window: { devicePixelRatio: 1 }, performance: { now: () => 0 }, AbortController,
    setTimeout: () => 0, clearTimeout() {}, setInterval: () => 0,
    ResizeObserver: class { observe() {} },
    fetch(url) { requests.push(url); return new Promise(() => {}); },
  });
  for (const file of ["observer-model.js", "observer-memory.js", "viewer.js"])
    vm.runInContext(fs.readFileSync(path.join(staticDir, file), "utf8"), context, { filename: file });
  const native = JSON.parse(fs.readFileSync(path.join(__dirname, "fixtures/telemetry_v4_native.json"), "utf8"));
  context.testSnapshot = native;
  vm.runInContext("snapshot = testSnapshot; selectedId = 901000; showDetail();", context);
  const aiSections = ["memory-section", "needs-section", "decision-section", "role-section", "hunt-section", "economy-section"];
  const index = fs.readFileSync(path.join(staticDir, "index.html"), "utf8");
  for (const id of aiSections) { assert(index.includes(`id="${id}"`)); assert.equal(elements.get(id).hidden, true, id); }
  assert.match(elements.get("detail-type").textContent, /běžné NPC/);
  assert.equal(elements.get("detail-health").textContent, "Zdraví 100 / 100");
  assert.equal(elements.get("detail-combat").textContent, "Mimo boj");
  assert.equal(elements.get("needs").children.length, 0);
  assert.match(elements.get("movement-facts").textContent, /Domov/);
  assert.deepEqual(requests, ["/api/state"]); // Native selection never asks for AI memory.
  vm.runInContext("selectedId = 901093; showDetail();", context);
  assert.equal(elements.get("detail-health").textContent, "Zdraví —");
  assert.equal(elements.get("detail-combat").textContent, "Boj —");
  assert.match(elements.get("detail-availability").textContent, /není načtené/);
  assert.equal(elements.get("detail-state").textContent, "—");
  const ai = JSON.parse(fs.readFileSync(path.join(__dirname, "fixtures/telemetry_v2.json"), "utf8"));
  context.testSnapshot = ai;
  vm.runInContext("snapshot = testSnapshot; selectedId = snapshot.agents[0].agent_id; showDetail();", context);
  for (const id of aiSections) assert.equal(elements.get(id).hidden, false, id);
  assert.equal(elements.get("needs").children.length, 5);
  assert(!elements.get("detail-type").textContent.includes("běžné NPC"));
});
