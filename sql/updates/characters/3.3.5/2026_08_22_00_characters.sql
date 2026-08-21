--
-- AIWorld: persistent long-term memories (Milestone 2.5B). memory_id is
-- MySQL AUTO_INCREMENT, read back the same way ai_agents.agent_id is (this
-- database layer has no last-insert-id support) - except long-term memory
-- inserts are async (CONNECTION_ASYNC/Execute(), not DirectExecute()), so
-- this process never reads a freshly-inserted row's id back at all; it's
-- only observed on the next LoadLongTermMemories() (a later startup).
--
-- ObjectGuid columns store ObjectGuid::GetRawValue() - reconstruct with
-- ObjectGuid::SetRawValue(), never treat as meaningful on their own.
-- actor/target identity should prefer AgentId, then (map_id, spawn_id),
-- then entry - the stored GUID is a historical snapshot, not a stable key.
--

CREATE TABLE IF NOT EXISTS `ai_long_term_memories` (
    `memory_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `agent_id` BIGINT UNSIGNED NOT NULL,

    `observation_type` TINYINT UNSIGNED NOT NULL,
    `importance` FLOAT NOT NULL,

    `source_event_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `has_source_event_type` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `source_event_type` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `correlation_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,

    `source_occurred_at_ms` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `first_observed_at_ms` BIGINT UNSIGNED NOT NULL,
    `last_observed_at_ms` BIGINT UNSIGNED NOT NULL,
    `observation_count` INT UNSIGNED NOT NULL DEFAULT 1,

    `map_id` INT UNSIGNED NOT NULL,
    `position_x` FLOAT NOT NULL,
    `position_y` FLOAT NOT NULL,
    `position_z` FLOAT NOT NULL,

    `actor_guid` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `actor_spawn_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `actor_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `actor_agent_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,

    `target_guid` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `target_spawn_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `target_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `target_agent_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,

    `channel` TINYINT UNSIGNED NOT NULL,

    PRIMARY KEY (`memory_id`),
    KEY `idx_ai_ltm_agent_time` (`agent_id`, `last_observed_at_ms`),
    KEY `idx_ai_ltm_agent_importance` (`agent_id`, `importance`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AIWorld persistent long-term memories';
