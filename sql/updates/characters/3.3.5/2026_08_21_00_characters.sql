--
-- AIWorld: persistent agent identity and world binding (Milestone 2.2A).
-- Only identity survives a restart here - agent_id is MySQL-assigned
-- (AUTO_INCREMENT) and read back by the unique (map_id, spawn_id) binding
-- after insert, since this database layer has no last-insert-id support.
-- RuntimeGuid/WorldState/SnapshotSequence are runtime-only and never
-- stored: every agent this table produces on load starts ABSTRACT.
--

CREATE TABLE IF NOT EXISTS `ai_agents` (
    `agent_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `agent_type` TINYINT UNSIGNED NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `spawn_id` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`agent_id`),
    UNIQUE KEY `uq_ai_agents_world_binding` (`map_id`, `spawn_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AIWorld persistent agent identity (Milestone 2.2A)';
