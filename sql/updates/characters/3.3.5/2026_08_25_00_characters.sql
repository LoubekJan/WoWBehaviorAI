--
-- AIWorld: persistent HomeLocation/WorkLocation (Milestone 2.11A). Optional
-- on every agent - most (e.g. GUARD) have neither, so all ten columns are
-- nullable and always set/read together in pairs (home_map_id NULL means
-- no HomeLocation at all, never a partially-set one). Profession/behavior
-- is deliberately not a new AgentType - a CIVILIAN with both locations set
-- is how "has a home and a workplace" is expressed. 2.11A only persists
-- and loads these; nothing acts on them yet (no MOVE_TO, no WORK goal, no
-- daily schedule).
--

ALTER TABLE `ai_agents`
    ADD COLUMN `home_map_id` INT UNSIGNED NULL DEFAULT NULL AFTER `spawn_id`,
    ADD COLUMN `home_x` FLOAT NULL DEFAULT NULL AFTER `home_map_id`,
    ADD COLUMN `home_y` FLOAT NULL DEFAULT NULL AFTER `home_x`,
    ADD COLUMN `home_z` FLOAT NULL DEFAULT NULL AFTER `home_y`,
    ADD COLUMN `home_o` FLOAT NULL DEFAULT NULL AFTER `home_z`,
    ADD COLUMN `work_map_id` INT UNSIGNED NULL DEFAULT NULL AFTER `home_o`,
    ADD COLUMN `work_x` FLOAT NULL DEFAULT NULL AFTER `work_map_id`,
    ADD COLUMN `work_y` FLOAT NULL DEFAULT NULL AFTER `work_x`,
    ADD COLUMN `work_z` FLOAT NULL DEFAULT NULL AFTER `work_y`,
    ADD COLUMN `work_o` FLOAT NULL DEFAULT NULL AFTER `work_z`;

--
-- Pa Maclure (entry 250, spawn 80683, map 0): the first persistent CIVILIAN
-- with both locations set. Idempotent against the existing
-- uq_ai_agents_world_binding (map_id, spawn_id) key, the same "reuse the
-- existing row instead of inserting a duplicate" rule AgentPersistence::
-- CreateCreatureAgent() already applies at the application layer.
--

INSERT INTO `ai_agents`
    (`agent_type`, `map_id`, `spawn_id`, `home_map_id`, `home_x`, `home_y`, `home_z`, `home_o`, `work_map_id`, `work_x`, `work_y`, `work_z`, `work_o`)
VALUES
    (0, 0, 80683, 0, -10005.200195, 52.820202, 34.653900, 0.296706, 0, -9968.537109, 7.791809, 33.738487, 0.544631)
ON DUPLICATE KEY UPDATE
    `agent_type` = VALUES(`agent_type`),
    `home_map_id` = VALUES(`home_map_id`),
    `home_x` = VALUES(`home_x`),
    `home_y` = VALUES(`home_y`),
    `home_z` = VALUES(`home_z`),
    `home_o` = VALUES(`home_o`),
    `work_map_id` = VALUES(`work_map_id`),
    `work_x` = VALUES(`work_x`),
    `work_y` = VALUES(`work_y`),
    `work_z` = VALUES(`work_z`),
    `work_o` = VALUES(`work_o`);
