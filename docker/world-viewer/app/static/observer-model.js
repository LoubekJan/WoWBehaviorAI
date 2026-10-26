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
      (!f.query || `${agent.name || ""} ${agent.agent_id} ${agent.spawn_id}`.toLocaleLowerCase("cs").includes(f.query)) &&
      (!f.type || agent.type === f.type) && (!f.faction || String(agent.world_faction) === f.faction) &&
      (!f.role || (!agent.native_creature && (agent.living_wolf ? "WOLF_PACK" : agent.living_role?.role) === f.role)) &&
      (!f.phase || phase(agent) === f.phase) && (!f.group || groupIds(agent).includes(f.group)) &&
      (!f.live || agent.position.source === "live") && (!f.combat || agent.in_combat === true) &&
      (!f.goal || Boolean(goal(agent))) && (!f.hunger || (!agent.native_creature && agent.needs?.hunger > 0.7)) &&
      (!f.alert || alert(agent)) && (!f.blocked || blocked(agent));
  }
  function aiDetailsSupported(agent) { return agent.native_creature !== true; }
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
      ACTOR_DEAD: "Mrtvý", UNLOADED: "Nenačtený" };
    const consumer = supply.role === "CONSUMER";
    const rows = [["Zásobování", consumer ? "Spotřebitel" : supply.role === "HUNTER" ? "Lovec" : "Predátor"],
      ["Právě dělá", phases[supply.phase] || supply.phase],
      ["Náklad", consumer ? null : `${supply.cargo} / ${supply.capacity} porcí`],
      ["Zbývá v kořisti", supply.carcass_remaining == null ? null : `${supply.carcass_remaining} porcí`],
      ["Získáno z kořisti", consumer ? null : supply.harvested], ["Doručeno", consumer ? null : supply.delivered],
      ["Snědeno", supply.consumed], ["Ztracený náklad", consumer ? null : supply.lost]];
    if (depot) rows.push(["Zásobárna", depot.name], ["Zásoba jídla", `${depot.stock} / ${depot.capacity} porcí`],
      ["Stav zásob", depot.stock === 0 ? "Prázdná" : depot.stock <= depot.low_threshold ? "Nízká zásoba" : "Dostatek"],
      ["Do skladu doručeno", depot.delivered], ["Ze skladu snědeno", depot.consumed]);
    if (accounting) rows.push(["Bilance potravy", accounting.conserved ? "Souhlasí" : "Nesouhlasí"],
      ["Ve skladech / v nákladech", `${accounting.in_depots} / ${accounting.in_cargo}`]);
    if (supply.last_result) rows.push(["Poslední výsledek", supply.last_result]);
    return rows;
  }
  function foodDepots(agents, mapId) {
    const depots = new Map();
    for (const agent of agents) {
      const depot = supplySupported(agent) ? agent.supply.depot : null;
      if (depot?.position?.map_id === mapId && !depots.has(depot.id)) depots.set(depot.id, depot);
    }
    return [...depots.values()].sort((a, b) => a.id - b.id);
  }
  return { goal, phase, groupIds, alert, blocked, matches, viewport, project, phaseColors, aiDetailsSupported,
    supplySupported, supplyFacts, foodDepots };
})();
if (typeof module !== "undefined") module.exports = ObserverModel;
