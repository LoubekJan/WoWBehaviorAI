--
-- AIWorld: CreatureGroup -> AgentGroup domain model rewrite (Milestone
-- 2.12C). A group is a social layer over independent member agents (each
-- with its own AgentId/identity/memory/needs/goal/decision/actions/
-- Creature lifecycle), never an aggregate that stands in for them - see
-- AgentGroupState.h/AgentGroupMembership.h for the full reasoning.
--
-- population is dropped: member count is never stored, only ever derived
-- as membership.size() at read time. kind (0 = Loose, 1 = Stable) joins
-- territory_x/y/z/hunger/resources as part of the same all-or-nothing
-- group-presence check AgentPersistence::LoadAgents() already enforces -
-- nullable, NULL unless this agent actually has an AgentGroupState.
--
-- ai_creature_group_members (map_id/spawn_id-keyed - membership by raw
-- creature spawn, back when members were not yet independent agents) is
-- replaced outright by ai_agent_group_members (member_agent_id-keyed -
-- membership by AgentId, matching every other agent relationship in this
-- schema). This is a fresh dev environment with only the one seeded test
-- group; there is no real membership data to migrate.
--

ALTER TABLE `ai_agents`
    DROP COLUMN `population`,
    ADD COLUMN `kind` TINYINT UNSIGNED NULL DEFAULT NULL AFTER `resources`;

DROP TABLE IF EXISTS `ai_creature_group_members`;

CREATE TABLE IF NOT EXISTS `ai_agent_group_members` (
    `group_agent_id` BIGINT UNSIGNED NOT NULL,
    `member_agent_id` BIGINT UNSIGNED NOT NULL,
    `joined_at_ms` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`group_agent_id`, `member_agent_id`),
    KEY `idx_ai_agent_group_members_member` (`member_agent_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AIWorld persistent AgentGroup membership (Milestone 2.12C)';

--
-- Fix up the 2.12A test group (spawn_id 1,000,000,001): kind above
-- defaults to NULL for every existing row, including this one, which
-- would otherwise leave it partially NULL (territory_*/hunger/resources
-- set, kind not) - exactly the malformed-row case AgentPersistence::
-- LoadAgents() rejects. Seeded as STABLE (1): a deliberately-authored,
-- persistent test group, not one expected to drift apart on its own.
--

UPDATE `ai_agents`
SET `kind` = 1
WHERE `agent_type` = 3 AND `map_id` = 0 AND `spawn_id` = 1000000001;
