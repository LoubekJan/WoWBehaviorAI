--
-- AIWorld: AgentGroup identity/state separated out of ai_agents into its
-- own table (Milestone 2.12D, STATIC review P2 fix on c2864d10a50f). A
-- group is a social layer over independent member agents, never an
-- aggregate sharing AgentId/(map_id, spawn_id) identity with them - see
-- GroupId.h/AgentGroupRecord.h/AgentGroupRegistry.h for the full
-- reasoning. AgentType::AgentGroup (value 3) is retired: every ai_agents
-- row from here on names a real, individually-bindable creature spawn.
--

CREATE TABLE IF NOT EXISTS `ai_agent_groups` (
    `group_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `kind` TINYINT UNSIGNED NOT NULL,
    `territory_map_id` INT UNSIGNED NOT NULL,
    `territory_x` FLOAT NOT NULL,
    `territory_y` FLOAT NOT NULL,
    `territory_z` FLOAT NOT NULL,
    `resources` FLOAT NOT NULL DEFAULT 0,
    `version` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`group_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AIWorld persistent AgentGroup identity/state (Milestone 2.12D)';

--
-- Migrate the one existing test group (agent_type = 3, map_id = 0,
-- spawn_id = 1000000001) out of ai_agents and into its own row here,
-- carrying over whatever kind/territory_*/resources/group_version it
-- currently holds - not hardcoded seed values, since 2.12B/2.12C testing
-- may already have drifted resources/group_version away from them. hunger
-- is deliberately not carried over: it no longer exists on
-- AgentGroupRecord (see AgentGroupRecord.h for why a group-level Hunger
-- was itself a symptom of the aggregate-replaces-members model this
-- rename moves away from). This is a fresh dev environment with only this
-- one seeded group (see the 2026_08_25_08 migration's own comment) -
-- group_id is deterministically 1, the first AUTO_INCREMENT value into a
-- table that starts empty with exactly one row inserted here.
--

INSERT INTO `ai_agent_groups`
    (`kind`, `territory_map_id`, `territory_x`, `territory_y`, `territory_z`, `resources`, `version`)
SELECT
    `kind`, `map_id`, `territory_x`, `territory_y`, `territory_z`, `resources`, `group_version`
FROM `ai_agents`
WHERE `agent_type` = 3 AND `map_id` = 0 AND `spawn_id` = 1000000001;

--
-- ai_agent_group_members: group_agent_id (an AgentId, borrowed from the
-- old aggregate model) is renamed to group_id and repointed at the new
-- ai_agent_groups.group_id above - a member's own identity
-- (member_agent_id) is untouched, it was already a real AgentId. Must run
-- before the DELETE below, while ai_agents still has the test group's row
-- to resolve its old agent_id from.
--

ALTER TABLE `ai_agent_group_members`
    CHANGE COLUMN `group_agent_id` `group_id` BIGINT UNSIGNED NOT NULL;

UPDATE `ai_agent_group_members`
SET `group_id` = 1
WHERE `group_id` = (SELECT `agent_id` FROM `ai_agents` WHERE `agent_type` = 3 AND `map_id` = 0 AND `spawn_id` = 1000000001);

--
-- Drop the migrated test group's own ai_agents row, then the now-unused
-- group-presence columns 2.12A/2.12B/2.12C added to ai_agents - an
-- AgentGroup is no longer represented there at all.
--

DELETE FROM `ai_agents`
WHERE `agent_type` = 3 AND `map_id` = 0 AND `spawn_id` = 1000000001;

ALTER TABLE `ai_agents`
    DROP COLUMN `territory_x`,
    DROP COLUMN `territory_y`,
    DROP COLUMN `territory_z`,
    DROP COLUMN `hunger`,
    DROP COLUMN `resources`,
    DROP COLUMN `kind`,
    DROP COLUMN `group_version`;
