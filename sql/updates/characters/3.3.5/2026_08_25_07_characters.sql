--
-- AIWorld: persistent CreatureGroup membership (Milestone 2.12C). Which
-- real TrinityCore creature spawns belong to a CreatureGroup aggregate -
-- nothing else. No FK to ai_agents (this database layer's established
-- convention, see ai_long_term_memories' own comment) - AgentPersistence::
-- LoadCreatureGroupMembers() validates group_agent_id against an already-
-- loaded, already-registered AgentType::CreatureGroup agent itself and
-- skips (logs) anything that doesn't resolve, the same orphan-row
-- tolerance ai_long_term_memories already has.
--
-- (map_id, spawn_id) is the primary key, not (group_agent_id, map_id,
-- spawn_id): a given real creature spawn can only ever belong to one
-- group. There is deliberately no runtime write path for this table in
-- 2.12C - membership is seeded/edited directly, the same way
-- HomeLocation/WorkLocation and the 2.12A test group's own row are.
--

CREATE TABLE IF NOT EXISTS `ai_creature_group_members` (
    `group_agent_id` BIGINT UNSIGNED NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `spawn_id` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`map_id`, `spawn_id`),
    KEY `idx_ai_creature_group_members_group` (`group_agent_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AIWorld persistent CreatureGroup membership (Milestone 2.12C)';
