--
-- AIWorld: CreatureGroup coarse-simulation state (Milestone 2.12B).
-- hunger/resources are nullable, part of the same all-or-nothing group
-- presence check population/territory_* already use (see AgentPersistence::
-- LoadAgents()'s own comment - all six must agree, all NULL or all set).
-- group_version is NOT NULL like economy_version - bookkeeping present on
-- every row regardless of whether this agent has a CreatureGroupState at
-- all, the async-write monotonic guard for
-- AgentPersistence::SaveCreatureGroupState().
--

ALTER TABLE `ai_agents`
    ADD COLUMN `hunger` FLOAT NULL DEFAULT NULL AFTER `territory_z`,
    ADD COLUMN `resources` FLOAT NULL DEFAULT NULL AFTER `hunger`,
    ADD COLUMN `group_version` BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER `resources`;

--
-- Fix up the 2.12A test group (spawn_id 1,000,000,001): the new hunger/
-- resources columns above default to NULL for every existing row,
-- including this one, which would otherwise leave it partially NULL
-- (population/territory_* set, hunger/resources not) - exactly the
-- malformed-row case AgentPersistence::LoadAgents() now detects and
-- rejects. Resources starts at 1.0 (full/undepleted), not 0.0, so
-- 2.12B's own downward drift is actually observable rather than already
-- clamped at its floor from the first tick.
--

UPDATE `ai_agents`
SET `hunger` = 0.0, `resources` = 1.0
WHERE `agent_type` = 3 AND `map_id` = 0 AND `spawn_id` = 1000000001;
