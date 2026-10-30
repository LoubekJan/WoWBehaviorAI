"use strict";

// Pure presentation rules shared by the browser and Node regression tests.
const ObserverModel = (() => {
  const phaseColors = {
    HUNTING: "#f0a060", FEEDING: "#d6bc70", DEFENDING: "#ed786c",
    FLEEING: "#dd8bc8", SEEKING_SAFETY: "#cba4ed", INVESTIGATING: "#7cbdeb",
    MOVING: "#7fc8ce", ACTING: "#a5d281", RESTING: "#a2a9e0", IDLE: "#afc0b2",
    DEAD: "#77827b", COMBAT: "#ed786c",
  };
  function goal(agent) {
    if (agent.native_creature) return null;
    return agent.effective_goal || agent.goal || agent.routine_goal || agent.coordination_goal || null;
  }
  function phase(agent) {
    if (agent.position.source !== "live") return agent.native_creature ? "NOT_LOADED" : "BACKGROUND";
    if (agent.alive === false) return "DEAD";
    if (agent.native_creature) {
      if (agent.in_combat === true) return "COMBAT";
      if (agent.movement?.moving === true) return "MOVING";
      return agent.alive === true ? "IDLE" : "UNKNOWN";
    }
    if (agent.living_wolf) {
      return ({ HUNT: "HUNTING", FEED: "FEEDING", DEFEND: "DEFENDING",
        FLEE_DANGER: "FLEEING", WILDLIFE_REST: "RESTING", ROAM: "MOVING", REGROUP: "MOVING" })[goal(agent)] || "IDLE";
    }
    return agent.living_role?.phase || "UNKNOWN";
  }
  function groupIds(agent) {
    if (agent.native_creature) return [];
    return agent.groups?.length ? agent.groups.map(group => String(group.id)) :
      agent.group_id == null ? [] : [String(agent.group_id)];
  }
  function alert(agent) {
    if (agent.position.source !== "live" || agent.alive === false) return false;
    if (agent.native_creature) return agent.in_combat === true;
    const role = agent.living_role;
    return agent.in_combat === true || ["DEFENDING", "FLEEING", "SEEKING_SAFETY", "INVESTIGATING"].includes(phase(agent)) ||
      Boolean(role && ((role.awareness && role.awareness !== "QUIET") || role.danger_remaining_ms > 0 || role.alarm_remaining_ms > 0));
  }
  function blocked(agent) { return Boolean(agent.movement?.blocked || agent.movement?.cannot_reach_target); }
  function viewport(scope) {
    if (!scope?.bounds) return { north: -8100, south: -10250, west: 900, east: -1750 };
    return { north: scope.bounds.max_x, south: scope.bounds.min_x,
      west: scope.bounds.max_y, east: scope.bounds.min_y };
  }
  function project(position, bounds, width, height, zoom = 1, panX = 0, panY = 0) {
    const scale = Math.min(width / (bounds.west - bounds.east), height / (bounds.north - bounds.south)) * 0.92 * zoom;
    return { x: width / 2 + ((bounds.west + bounds.east) / 2 - position.y) * scale + panX,
      y: height / 2 + ((bounds.north + bounds.south) / 2 - position.x) * scale + panY };
  }
  function matches(agent, f, scope = { map_id: 0 }) {
    // Keep escaped points on this map visible; bounds govern simulation and
    // viewport size, not whether the observer retains the diagnostic.
    return agent.position.map_id === scope.map_id &&
      (!f.query || `${agent.name || ""} ${agent.llm_agent?.name || ""} ${agent.agent_id} ${agent.spawn_id}`.toLocaleLowerCase("cs").includes(f.query)) &&
      (!f.type || agent.type === f.type) && (!f.faction || String(agent.world_faction) === f.faction) &&
      (!f.role || (!agent.native_creature && (agent.living_wolf ? "WOLF_PACK" : agent.living_role?.role) === f.role)) &&
      (!f.phase || phase(agent) === f.phase) && (!f.group || groupIds(agent).includes(f.group)) &&
      (!f.live || agent.position.source === "live") && (!f.combat || agent.in_combat === true) &&
      (!f.goal || Boolean(goal(agent))) && (!f.hunger || (!agent.native_creature && agent.needs?.hunger > 0.7)) &&
      (!f.alert || alert(agent)) && (!f.blocked || blocked(agent));
  }
  function aiDetailsSupported(agent) { return agent.native_creature !== true; }
  function npcAgentSupported(agent) {
    return agent.native_creature !== true && agent.position.source === "live" && Boolean(agent.llm_agent);
  }
  function npcAgentFacts(agent) {
    if (!npcAgentSupported(agent)) return [];
    const npc = agent.llm_agent;
    return [["Jméno", npc.name], ["Stav rozhodování", npc.status], ["Probíhající činnost", npc.active_action],
      ["Poslední výsledek", npc.last_action_result], ["Požadavek", npc.request_id === "0" ? null : npc.request_id],
      ["Další rozhodnutí", `${npc.next_think_ms} ms`], ["Zapamatované repliky", `${npc.dialogue_count} / 12`],
      ["Vzpomínky", `${npc.memory_records ?? 0} / 96`], ["Vybavené vzpomínky", `${npc.memory_recalled ?? 0} / 6`]];
  }
  function supplySupported(agent) {
    return agent.native_creature !== true && agent.position.source === "live" && agent.supply?.enabled === true;
  }
  function supplyFacts(agent) {
    if (!supplySupported(agent)) return [];
    const supply = agent.supply, depot = supply.depot, accounting = supply.accounting;
    const phases = { IDLE: "Čeká", WAITING: "Čeká", TO_GROUND: "Cesta na loviště",
      TO_HUNTING_GROUND: "Cesta na loviště", SEARCHING: "Hledání kořisti", HUNTING: "Lov",
      HARVESTING: "Zpracování kořisti", CARRYING: "Nese potravu", TO_DEPOT: "Cesta do skladu",
      RETURNING_TO_DEPOT: "Cesta do skladu", DELIVERING: "Vykládání", EATING_DEPOT: "Jídlo ze skladu",
      FEEDING: "Krmení z kořisti", FED: "Nakrmený", RESTING: "Odpočinek", NO_ROUTE: "Cesta nedostupná",
      RETURNING_HOME: "Návrat k běžné činnosti", RETURN_HOME: "Návrat k běžné činnosti",
      ORDINARY: "Běžná činnost", NORMAL: "Běžná činnost", NO_FOOD: "Čeká na jídlo", WAITING_FOR_FOOD: "Čeká na jídlo",
      ACTOR_DEAD: "Mrtvý", UNLOADED: "Nenačtený", TO_FIELD: "Cesta na pole", WORKING_FIELD: "Práce na poli",
      HARVESTED_FIELD: "Sklizeň", FIELD_MAINTENANCE: "Péče o pole", READY_FOR_FIELD: "Připraven na pole",
      RESTING_HOME: "Odpočinek doma", TO_WAREHOUSE: "Cesta do vlastního skladu" };
    const consumer = supply.role === "CONSUMER", raider = supply.role === "RAIDER", farmer = supply.role === "FARMER";
    const rows = [["Zásobování", consumer ? "Spotřebitel" : raider ? "Nájezdník" : farmer ? "Zemědělec" : supply.role === "HUNTER" ? "Lovec" : "Predátor"],
      ["Právě dělá", phases[supply.phase] || supply.phase],
      ["Náklad", consumer ? null : `${supply.cargo} / ${supply.capacity} porcí`],
      ["Zbývá v kořisti", supply.carcass_remaining == null ? null : `${supply.carcass_remaining} porcí`],
      [farmer ? "Sklizeno na poli" : "Získáno z kořisti", consumer || raider ? null : supply.harvested],
      ["Odcizené jídlo", raider ? supply.stolen : null], ["Doručeno", consumer ? null : supply.delivered],
      ["Snědeno", supply.consumed], ["Ztracený náklad", consumer ? null : supply.lost]];
    if (farmer) rows.push(["Pole", supply.field_id], ["Pracovní místo", supply.field_workpoint_index == null ? null : supply.field_workpoint_index + 1],
      ["Zralá úroda", supply.field_remaining == null ? null : `${supply.field_remaining} / ${supply.field_capacity} porcí`]);
    if (depot) rows.push(["Zásobárna", depot.name], ["Zásoba jídla", `${depot.stock} / ${depot.capacity} porcí`],
      ["Stav zásob", depot.stock === 0 ? "Prázdná" : depot.stock <= depot.low_threshold ? "Nízká zásoba" : "Dostatek"],
      ["Do skladu doručeno", depot.delivered], ["Ze skladu snědeno", depot.consumed],
      ["Ze skladu odcizeno", depot.stolen || "0"]);
    if (accounting) rows.push(["Bilance potravy", accounting.conserved ? "Souhlasí" : "Nesouhlasí"],
      ["Ve skladech / v nákladech", `${accounting.in_depots} / ${accounting.in_cargo}`]);
    if (supply.last_result) rows.push(["Poslední výsledek", supply.last_result]);
    return rows;
  }
  function raidSupported(agent) {
    return supplySupported(agent) && agent.raid?.enabled === true;
  }
  function raidFacts(agent) {
    if (!raidSupported(agent)) return [];
    const raid = agent.raid;
    const phases = { IDLE: "Čeká", WAITING: "Čeká", WAITING_SUPPLY: "Čeká na zásoby ve vesnici", COOLDOWN: "Čeká na další pokus",
      WAIT_DANGER: "Odkládá výpravu kvůli nebezpečí", DEFERRED: "Odkládá výpravu podle zkušeností",
      WAITING_MEMORY: "Odkládá výpravu podle zkušeností",
      TO_VILLAGE: "Cesta k vesnici", TO_SOURCE: "Cesta k zásobárně", STEALING: "Odcizování zásob",
      CARRYING: "Odnáší zásoby", TO_CAMP: "Cesta do tábora", RETURNING_CAMP: "Cesta do tábora",
      DELIVERING: "Vykládání zásob", DELIVERING_CAMP: "Vykládání zásob",
      EATING_CAMP: "Jídlo v táboře", RETURNING_EMPTY: "Vrací se bez kořisti", FLEEING: "Ustupuje před nebezpečím",
      PATROLLING: "Hlídka", GUARD_HOME: "U domova strážného", GUARD_POST: "Na strážním místě",
      NOTICED: "Zpozoroval nájezdníka", INTERCEPTING: "Zasahuje",
      PREVENTED: "Krádež zmařena", ACTOR_DEAD: "Mrtvý", UNLOADED: "Nenačtený" };
    const rows = [["Úloha při nájezdu", raid.role === "RAIDER" ? "Nájezdník" : "Strážný"],
      ["Právě dělá", phases[raid.phase] || raid.phase], ["Pokus", raid.attempt_id],
      ["Výsledek", raid.outcome], ["Protějšek NPC", raid.target_spawn_id],
      ["Zpozorování", raid.noticed], ["Zásahy stráže", raid.interceptions],
      ["Dokončené krádeže", raid.thefts], ["Doručení do tábora", raid.delivered], ["Zmařené pokusy", raid.prevented],
      [raid.role === "RAIDER" ? "Cílový sklad" : "Střežený sklad", raid.source_depot?.name],
      ["Skutečné zásoby skladu", raid.source_depot?.stock], ["Z tohoto skladu odcizeno", raid.source_depot?.stolen || "0"],
      ["Tábor", raid.camp_depot?.name], ["Jídlo v táboře", raid.camp_depot?.stock]];
    const plan = raid.role === "RAIDER" ? raid.planning : null;
    if (plan) {
      const reasons = { EXPLORE_UNKNOWN: "Ověřit dosud neznámé zásoby", KNOWN_STOCK: "Poslední zjištěné zásoby",
        WAIT_DANGER: "Počkat kvůli zapamatovanému nebezpečí", WAIT_EMPTY: "Počkat po zjištění prázdných skladů",
        ALL_ROUTES_DANGEROUS: "Známé cesty jsou nebezpečné", AVOID_DANGER: "Vyhnout se zapamatovanému nebezpečí",
        KNOWN_SUCCESS: "Předchozí úspěšná výprava", NO_MEMORY: "Zatím nemá zkušenost", LEGACY: "Původní přepad",
        REMEMBERED_SUPPLY: "Poslední zjištěné zásoby", EXPLORE_UNKNOWN_STOCK: "Ověřit dosud neznámé zásoby",
        DEFER_REMEMBERED_DANGER: "Počkat kvůli zapamatovanému nebezpečí", DEFER_KNOWN_EMPTY: "Počkat po zjištění prázdných skladů",
        AVOID_REMEMBERED_DANGER: "Vyhnout se zapamatovanému nebezpečí", NO_CANDIDATE: "Zatím nemá vhodný cíl",
        NOT_EVALUATED: "Zatím neplánuje výpravu",
        INVALID_CONTEXT: "Plán nemá platné podklady" };
      const seconds = value => `${(value / 1000).toFixed(1)} s`;
      rows.push(["Důvod plánu", reasons[plan.reason] || plan.reason], ["Vzpomínky na výpravy", plan.memory_records],
        ["Zvolená cesta", ({ 0: "Zatím nevybraná", 1: "Původní cesta", 2: "Obchvat levé vesnice" })[plan.route_id] || plan.route_id],
        ["NPC naposledy zjistilo", plan.known_stock ?? "Zásoby dosud nezná"],
        ["Stáří informace o zásobách", plan.stock_age_ms == null ? null : seconds(plan.stock_age_ms)],
        ["Riziko podle paměti", plan.risk >= 4 ? "Vysoké" : plan.risk > 0 ? "Nižší" : "Žádné známé"],
        ["Další plánování za", plan.retry_remaining_ms > 0 ? seconds(plan.retry_remaining_ms) : null]);
      if (plan.danger) {
        const danger = plan.danger, point = danger.position;
        rows.push(["Zapamatovaný útočník", danger.guard_spawn_id === "0" ? "Neznámý" : danger.guard_spawn_id],
          ["Místo nebezpečí", point ? `${point.x.toFixed(1)}, ${point.y.toFixed(1)}, ${point.z.toFixed(1)}` : null],
          ["Stáří nebezpečí", seconds(danger.age_ms)]);
      }
    }
    return rows;
  }
  function foodDepots(agents, mapId) {
    const depots = new Map();
    for (const agent of agents) {
      const copies = supplySupported(agent) ? [agent.supply.depot] : [];
      if (raidSupported(agent)) copies.push(agent.raid.source_depot, agent.raid.camp_depot);
      for (const depot of copies)
        if (depot?.position?.map_id === mapId && !depots.has(depot.id)) depots.set(depot.id, depot);
    }
    return [...depots.values()].sort((a, b) => a.id - b.id);
  }
  return { goal, phase, groupIds, alert, blocked, matches, viewport, project, phaseColors, aiDetailsSupported,
    npcAgentSupported, npcAgentFacts,
    supplySupported, supplyFacts, raidSupported, raidFacts, foodDepots };
})();
if (typeof module !== "undefined") module.exports = ObserverModel;
