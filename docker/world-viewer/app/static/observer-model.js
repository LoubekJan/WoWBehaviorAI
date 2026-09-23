"use strict";

// Pure presentation rules shared by the browser and Node regression tests.
const ObserverModel = (() => {
  const phaseColors = {
    HUNTING: "#f0a060", FEEDING: "#d6bc70", DEFENDING: "#ed786c",
    FLEEING: "#dd8bc8", SEEKING_SAFETY: "#cba4ed", INVESTIGATING: "#7cbdeb",
    MOVING: "#7fc8ce", ACTING: "#a5d281", RESTING: "#a2a9e0", IDLE: "#afc0b2",
    DEAD: "#77827b",
  };
  function goal(agent) {
    return agent.effective_goal || agent.goal || agent.routine_goal || agent.coordination_goal || null;
  }
  function phase(agent) {
    if (agent.position.source !== "live") return "BACKGROUND";
    if (agent.alive === false) return "DEAD";
    if (agent.living_wolf) {
      return ({ HUNT: "HUNTING", FEED: "FEEDING", DEFEND: "DEFENDING",
        FLEE_DANGER: "FLEEING", WILDLIFE_REST: "RESTING", ROAM: "MOVING", REGROUP: "MOVING" })[goal(agent)] || "IDLE";
    }
    return agent.living_role?.phase || "UNKNOWN";
  }
  function groupIds(agent) {
    return agent.groups?.length ? agent.groups.map(group => String(group.id)) :
      agent.group_id == null ? [] : [String(agent.group_id)];
  }
  function alert(agent) {
    if (agent.position.source !== "live" || agent.alive === false) return false;
    const role = agent.living_role;
    return agent.in_combat === true || ["DEFENDING", "FLEEING", "SEEKING_SAFETY", "INVESTIGATING"].includes(phase(agent)) ||
      Boolean(role && ((role.awareness && role.awareness !== "QUIET") || role.danger_remaining_ms > 0 || role.alarm_remaining_ms > 0));
  }
  function blocked(agent) { return Boolean(agent.movement?.blocked || agent.movement?.cannot_reach_target); }
  function matches(agent, f) {
    return agent.position.map_id === 0 &&
      (!f.query || `${agent.name || ""} ${agent.agent_id} ${agent.spawn_id}`.toLocaleLowerCase("cs").includes(f.query)) &&
      (!f.type || agent.type === f.type) && (!f.faction || String(agent.world_faction) === f.faction) &&
      (!f.role || (agent.living_wolf ? "WOLF_PACK" : agent.living_role?.role) === f.role) &&
      (!f.phase || phase(agent) === f.phase) && (!f.group || groupIds(agent).includes(f.group)) &&
      (!f.live || agent.position.source === "live") && (!f.combat || agent.in_combat === true) &&
      (!f.goal || Boolean(goal(agent))) && (!f.hunger || agent.needs.hunger > 0.7) &&
      (!f.alert || alert(agent)) && (!f.blocked || blocked(agent));
  }
  return { goal, phase, groupIds, alert, blocked, matches, phaseColors };
})();
if (typeof module !== "undefined") module.exports = ObserverModel;
