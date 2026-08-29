--
-- AIWorld: AgentRecord::ControlMode (Milestone 2.12F4A). Explicit,
-- persistent ownership state - previously, existence of an ai_agents row
-- implicitly meant AIWorldMgr::OwnsSpawn() would return true and
-- AIWorldCreatureAI would take over the Creature's AI (REACT_PASSIVE,
-- suppressed auto-aggro/auto-chase, TrinityCore's own Random/Waypoint
-- movement stopped). That conflation is unsafe the moment ai_agents rows
-- are ever registered for creatures other than a handful of hand-picked
-- test mobs (2.12F4's own later reconciliation step) - guards, vendors,
-- quest NPCs, bosses, and scripted NPCs must keep their normal TrinityCore/
-- scripted AI unless explicitly opted into AIWorld control.
--
-- 0 = ObserveOnly (AIWorld may observe/state-track, MUST NOT cause a
-- physical world mutation - the fail-closed default for every existing
-- and future row), 1 = AIWorldControlled (AIWorldCreatureAI may take over
-- CreatureAI, AIWorld's own Needs -> Goal -> ActionRequest -> ActionSystem
-- -> TrinityCore pipeline may actually execute). See AgentControlMode's
-- own comment (AgentType.h) for the full semantics - these numeric values
-- are storage ABI from here on, never renumbered or reused.
--

ALTER TABLE `ai_agents`
    ADD COLUMN `control_mode` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `spawn_id`;

--
-- Migrate the four existing, already-runtime-verified test agents to
-- AIWorldControlled - everything else (every row that predates this
-- column, and every row created from here on) stays at the column's own
-- ObserveOnly default. Bound by the persistent (map_id, spawn_id) world
-- binding, not agent_id - that binding is what actually describes the
-- migration's own intent ("these four specific spawns keep AIWorld
-- control"), the same identity AgentPersistence::CreateCreatureAgent()'s
-- own idempotency already keys on, rather than an incidentally-assigned
-- AUTO_INCREMENT id.
--

UPDATE `ai_agents`
SET `control_mode` = 1
WHERE (`map_id`, `spawn_id`) IN (
    (0, 80335),
    (0, 214023),
    (0, 214021),
    (0, 80683)
);
