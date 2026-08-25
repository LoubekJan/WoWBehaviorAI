--
-- AIWorld: persistent economy stockpile (Milestone 2.11E2). Distinct from
-- NeedsState::ResourcePressure (a 0.0-1.0 drive to act, drifting over
-- time) - these are actual accumulated counts, not nullable: every agent
-- has one, defaulting to 0 rather than "unset". Only money is currently
-- written to by AIWorldMgr (on a WORK ActionCompletion reaching
-- Succeeded/Performed); food/resource are added now so a later
-- milestone's production does not need another schema migration.
--

ALTER TABLE `ai_agents`
    ADD COLUMN `money` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `work_o`,
    ADD COLUMN `food` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `money`,
    ADD COLUMN `resource` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `food`;
