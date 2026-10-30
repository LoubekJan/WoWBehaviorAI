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
  context.testSupply = { enabled: true, role: "HUNTER", phase: "DELIVERING", cargo: 6, capacity: 6,
    harvested: "6", delivered: "0", consumed: "0", lost: "0",
    depot: { id: 1, name: "Jižní vesnice", stock: 4, capacity: 24, low_threshold: 6, delivered: "0", consumed: "0" } };
  vm.runInContext("snapshot.agents[0].supply = testSupply; showDetail();", context);
  assert.equal(elements.get("supply-section").hidden, false);
  assert.match(elements.get("supply-facts").textContent, /Vykládání/);
  assert.match(elements.get("supply-facts").textContent, /6 \/ 6 porcí/);
  assert.match(elements.get("supply-facts").textContent, /Jižní vesnice/);
  assert.match(elements.get("supply-facts").textContent, /4 \/ 24 porcí/);
  context.testNpc = { name: "Matěj", status: "THINKING", active_action: "MOVE_TO", last_action_result: "ARRIVED",
    request_id: "18446744073709551615", next_think_ms: 2000, dialogue_count: 12,
    last_speech: "<img src=x onerror=alert(1)>" };
  vm.runInContext("snapshot.agents[0].llm_agent = testNpc; showDetail();", context);
  assert.equal(elements.get("npc-agent-section").hidden, false);
  assert.equal(elements.get("detail-name").textContent, "Matěj");
  assert.match(elements.get("npc-agent-facts").textContent, /THINKING/);
  assert.match(elements.get("npc-agent-facts").textContent, /18446744073709551615/);
  assert.equal(elements.get("npc-agent-speech").textContent, "<img src=x onerror=alert(1)>");
  context.testSnapshot = native;
  vm.runInContext("snapshot = testSnapshot; selectedId = 901000; showDetail();", context);
  assert.equal(elements.get("supply-section").hidden, true);
  assert.equal(elements.get("supply-facts").textContent, "");
  assert.equal(elements.get("npc-agent-section").hidden, true);
  assert.equal(elements.get("npc-agent-facts").textContent, "");
  assert.equal(elements.get("npc-agent-speech").textContent, "—");
});

test("supply facts preserve exact counters and disappear without current controlled evidence", () => {
  const hunter = { ...base, supply: { enabled: true, role: "HUNTER", phase: "TO_DEPOT", cargo: 6, capacity: 6,
    harvested: "18446744073709551615", delivered: "8", consumed: "2", lost: "0", carcass_remaining: 0,
    depot: { name: "Gnollí tábor", stock: 0, capacity: 24, low_threshold: 6, delivered: "8", consumed: "8" },
    accounting: { conserved: false, in_depots: "0", in_cargo: "6" } } };
  const rows = Object.fromEntries(model.supplyFacts(hunter));
  assert.equal(rows["Právě dělá"], "Cesta do skladu");
  assert.equal(rows["Získáno z kořisti"], "18446744073709551615");
  assert.equal(rows["Zbývá v kořisti"], "0 porcí");
  assert.equal(rows["Stav zásob"], "Prázdná");
  assert.equal(rows["Bilance potravy"], "Nesouhlasí");
  assert.deepEqual(model.supplyFacts(base), []);
  assert.deepEqual(model.supplyFacts({ ...hunter, native_creature: true }), []);
  assert.deepEqual(model.supplyFacts({ ...hunter, position: { ...base.position, source: "spawn" } }), []);
  assert.deepEqual(model.supplyFacts({ ...hunter, supply: { ...hunter.supply, enabled: false } }), []);
});

test("food depots are deduplicated current observations on the selected map", () => {
  const depot = { id: 2, name: "Gnollí tábor", position: { map_id: 725, x: 1, y: 2, z: 0 }, stock: 4, capacity: 24 };
  const hunter = { ...base, supply: { enabled: true, depot } };
  const other = { ...base, supply: { enabled: true, depot: { ...depot, id: 1, name: "Jižní vesnice" } } };
  assert.deepEqual(model.foodDepots([hunter, hunter, other], 725).map(item => item.id), [1, 2]);
  assert.deepEqual(model.foodDepots([hunter], 0), []);
  assert.deepEqual(model.foodDepots([{ ...hunter, native_creature: true }], 725), []);
  assert.deepEqual(model.foodDepots([{ ...hunter, position: { ...base.position, source: "spawn" } }], 725), []);
  assert.equal(model.foodDepots([hunter], 725)[0].stock, 4);
});

test("consumer facts show meals and ordinary work without invented hunting cargo", () => {
  const consumer = { ...base, supply: { enabled: true, role: "CONSUMER", phase: "ORDINARY", cargo: 0, capacity: 0,
    harvested: "0", delivered: "0", consumed: "2", lost: "0", last_result: "ATE_DEPOT",
    depot: { name: "Jižní vesnice", stock: 1, capacity: 30, low_threshold: 4, delivered: "6", consumed: "5" } } };
  const rows = Object.fromEntries(model.supplyFacts(consumer));
  assert.equal(rows["Zásobování"], "Spotřebitel");
  assert.equal(rows["Právě dělá"], "Běžná činnost");
  assert.equal(rows["Snědeno"], "2");
  assert.equal(rows["Zásobárna"], "Jižní vesnice");
  assert.equal(rows["Náklad"], null);
  assert.equal(rows["Získáno z kořisti"], null);
  assert.equal(rows["Doručeno"], null);
  consumer.supply.phase = "NO_FOOD";
  assert.equal(Object.fromEntries(model.supplyFacts(consumer))["Právě dělá"], "Čeká na jídlo");
});

test("raid facts retain attempt identity and share village and camp stock observations", () => {
  const source = { id: 1, name: "Jižní vesnice", stock: 4, position: { map_id: 725, x: 1, y: 2, z: 0 } };
  const camp = { id: 3, name: "Tábor Defias", stock: 2, position: { map_id: 725, x: 20, y: 30, z: 0 } };
  const raider = { ...base, supply: { enabled: true, role: "RAIDER", cargo: 2, capacity: 2, stolen: "2", depot: camp },
    raid: { enabled: true, role: "RAIDER", phase: "STEALING", attempt_id: "18446744073709551615", outcome: "STOLEN",
      target_spawn_id: "901039", noticed: "1", interceptions: "0", thefts: "1", delivered: "0", prevented: "0",
      source_depot: source, camp_depot: camp } };
  const facts = Object.fromEntries(model.raidFacts(raider));
  assert.equal(facts["Pokus"], "18446744073709551615");
  assert.equal(facts["Právě dělá"], "Odcizování zásob");
  assert.equal(facts["Skutečné zásoby skladu"], 4);
  assert.equal(facts["Jídlo v táboře"], 2);
  assert.equal(Object.fromEntries(model.supplyFacts(raider))["Odcizené jídlo"], "2");
  assert.deepEqual(model.foodDepots([raider], 725).map(d => d.id), [1, 3]);
  assert.deepEqual(model.raidFacts({ ...raider, native_creature: true }), []);
  assert.deepEqual(model.raidFacts({ ...raider, position: { ...base.position, source: "spawn" } }), []);
  assert.deepEqual(model.raidFacts(base), []);
});

test("raid facts follow the selected village without merging stock or stolen portions", () => {
  const camp = { id: 3, name: "Tábor Defias", stock: 2, position: { map_id: 725, x: 20, y: 30, z: 0 } };
  const warehouses = [1, 4, 5, 6].map((id, index) => ({ id, name: `Vesnice ${id}`, stock: 10 + index,
    stolen: String(index * 2), position: { map_id: 725, x: id, y: id, z: 0 } }));
  const workers = warehouses.map(depot => ({ ...base, supply: { enabled: true, role: "CONSUMER", depot } }));
  const thief = { ...base, supply: { enabled: true, role: "RAIDER", stolen: "12", depot: camp },
    raid: { enabled: true, role: "RAIDER", phase: "TO_SOURCE", camp_depot: camp } };
  for (const source of warehouses) {
    thief.raid.source_depot = source;
    const facts = Object.fromEntries(model.raidFacts(thief));
    assert.equal(facts["Cílový sklad"], source.name);
    assert.equal(facts["Skutečné zásoby skladu"], source.stock);
    assert.equal(facts["Z tohoto skladu odcizeno"], source.stolen);
    assert.equal(Object.fromEntries(model.supplyFacts(thief))["Odcizené jídlo"], "12");
    assert.deepEqual(model.foodDepots([thief, ...workers], 725).map(d => [d.id, d.stock, d.stolen]),
      [warehouses[0], camp, ...warehouses.slice(1)].map(d => [d.id, d.stock, d.stolen]));
  }
  thief.raid.role = "GUARD";
  thief.raid.source_depot = warehouses[0];
  assert.equal(Object.fromEntries(model.raidFacts(thief))["Střežený sklad"], "Vesnice 1");
});

test("raid planning shows remembered route danger without inventing warehouse knowledge", () => {
  const source = { id: 6, name: "Pravá vesnice", stock: 30, position: { map_id: 725, x: 151, y: 282, z: 0.15 } };
  const thief = { ...base, supply: { enabled: true, role: "RAIDER" }, raid: { enabled: true, role: "RAIDER",
    phase: "TO_SOURCE", source_depot: source, planning: { reason: "EXPLORE_UNKNOWN", memory_records: 3,
      route_id: 2, known_stock: null, stock_age_ms: null, risk: 0.2, retry_remaining_ms: 0,
      danger: { guard_spawn_id: "901026", age_ms: 120000, position: { map_id: 725, x: 40, y: 873, z: 0.15 } } } } };
  let facts = Object.fromEntries(model.raidFacts(thief));
  assert.equal(facts["Cílový sklad"], "Pravá vesnice");
  assert.equal(facts["Skutečné zásoby skladu"], 30);
  assert.equal(facts["NPC naposledy zjistilo"], "Zásoby dosud nezná");
  assert.equal(facts["Stáří informace o zásobách"], null);
  assert.equal(facts["Zapamatovaný útočník"], "901026");
  assert.equal(facts["Místo nebezpečí"], "40.0, 873.0, 0.1");
  assert.equal(facts["Zvolená cesta"], "Obchvat levé vesnice");
  assert.equal(facts["Riziko podle paměti"], "Nižší");
  assert.equal(facts["Stáří nebezpečí"], "120.0 s");
  thief.raid.planning.known_stock = 0;
  thief.raid.planning.stock_age_ms = 5000;
  facts = Object.fromEntries(model.raidFacts(thief));
  assert.equal(facts["NPC naposledy zjistilo"], 0);
  assert.equal(facts["Stáří informace o zásobách"], "5.0 s");
  thief.raid.phase = "WAITING_MEMORY";
  thief.raid.planning.reason = "WAIT_DANGER";
  thief.raid.planning.risk = 20;
  thief.raid.planning.retry_remaining_ms = 10000;
  facts = Object.fromEntries(model.raidFacts(thief));
  assert.equal(facts["Právě dělá"], "Odkládá výpravu podle zkušeností");
  assert.equal(facts["Důvod plánu"], "Počkat kvůli zapamatovanému nebezpečí");
  assert.equal(facts["Riziko podle paměti"], "Vysoké");
  assert.equal(facts["Další plánování za"], "10.0 s");
  delete thief.raid.planning;
  assert.equal(Object.fromEntries(model.raidFacts(thief))["NPC naposledy zjistilo"], undefined);
});

function viewerPollingHarness() {
  const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
  class Element {
    constructor() { this.children = []; this.style = {}; this.value = ""; }
    addEventListener() {}
    replaceChildren(...children) { this.children = children; }
    append(...children) { this.children.push(...children); }
    getContext() { return {}; }
    setAttribute() {}
  }
  const elements = new Map(), requests = [], timers = [], intervals = [];
  let now = 10000;
  const context = vm.createContext({
    ObserverModel: model, ObserverMemoryPanel: class { select() {} },
    document: { getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
      createElement() { return new Element(); }, createTextNode(text) { return { textContent: text }; } },
    window: { devicePixelRatio: 1 }, performance: { now: () => now },
    Option: class { constructor(text, value) { this.textContent = text; this.value = value; } },
    ResizeObserver: class { observe() {} },
    setTimeout(callback, delay) { timers.push({ callback, delay }); },
    setInterval(callback) { intervals.push(callback); },
    fetch(url, options) { return new Promise((resolve, reject) => requests.push({ url, options, resolve, reject })); },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, "../app/static/viewer.js"), "utf8"), context);
  // These tests exercise the real asynchronous poll, status and snapshot state;
  // canvas painting and the unrelated detail panel have separate model tests.
  vm.runInContext("filterAgents = () => {}; showDetail = () => {};", context);
  return { context, elements, requests, timers, intervals,
    advance(milliseconds) { now += milliseconds; },
    snapshot() { return JSON.parse(vm.runInContext("JSON.stringify(snapshot)", context)); },
    poll() { return vm.runInContext("poll()", context); },
    next() { const timer = timers.shift(); timer.callback(); return timer.delay; },
  };
}

const flushPoll = () => new Promise(resolve => setImmediate(resolve));
function livePollSnapshot(x = 1, overrides = {}) {
  return { configured: true, stale: false, age_ms: 0, received_at_ms: 10000,
    agents: [{ ...base, type: "GUARD", world_faction: 1, position: { ...base.position, x, y: 2, z: 0 } }], ...overrides };
}

test("viewer polls fresh positions after 250ms without overlapping slow requests", async () => {
  const h = viewerPollingHarness();
  assert.equal(h.requests.length, 1);
  assert.equal(h.timers.length, 0); // A slow fetch cannot enqueue another poll.
  h.advance(1500);
  await h.poll(); await h.poll();
  assert.equal(h.requests.length, 1);
  assert.equal(h.timers.length, 0);
  h.requests[0].resolve({ ok: true, json: async () => livePollSnapshot(10.125) });
  await flushPoll();
  assert.equal(h.timers.length, 1);
  assert.equal(h.timers[0].delay, 250);
  assert.equal(h.snapshot().agents[0].position.x, 10.125);
  assert.equal(h.next(), 250);
  await h.poll();
  assert.equal(h.requests.length, 2);
  assert.equal(h.timers.length, 0);
  h.requests[1].resolve({ ok: true, json: async () => livePollSnapshot(11.375) });
  await flushPoll();
  assert.equal(h.snapshot().agents[0].position.x, 11.375);
  assert.equal(h.timers.length, 1);
  assert.equal(h.timers[0].delay, 250);
  for (const request of h.requests) {
    assert.equal(request.url, "/api/state");
    assert.equal(request.options.cache, "no-store");
  }
});

test("viewer backs off on HTTP, network and stale data and retains the five-second stale indicator", async () => {
  const h = viewerPollingHarness();
  h.requests[0].resolve({ ok: false, status: 503 });
  await flushPoll();
  assert.match(h.elements.get("status-text").textContent, /přerušeno/);
  assert.equal(h.next(), 1000);
  h.requests[1].resolve({ ok: true, json: async () => livePollSnapshot(7) });
  await flushPoll();
  assert.equal(h.elements.get("status-dot").className, "status-dot live");
  assert.equal(h.next(), 250);
  h.requests[2].reject(new Error("offline"));
  await flushPoll();
  assert.match(h.elements.get("status-text").textContent, /přerušeno/);
  assert.equal(h.snapshot().agents[0].position.x, 7);
  assert.equal(h.next(), 1000);
  h.requests[3].resolve({ ok: true, json: async () => livePollSnapshot(8, { stale: true, age_ms: 6000 }) });
  await flushPoll();
  assert.equal(h.elements.get("status-dot").className, "status-dot stale");
  assert.equal(h.next(), 1000);
  h.requests[4].resolve({ ok: true, json: async () => livePollSnapshot(9) });
  await flushPoll();
  assert.equal(h.elements.get("status-dot").className, "status-dot live");
  assert.equal(h.next(), 250);
  h.advance(5000);
  h.intervals[0]();
  assert.equal(h.elements.get("status-dot").className, "status-dot stale");
  await h.poll();
  assert.equal(h.requests.length, 6); // The pending request remains the only one.
  assert.equal(h.timers.length, 0);
});
