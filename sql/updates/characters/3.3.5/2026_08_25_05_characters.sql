--
-- AIWorld: persistent CreatureGroup aggregate state (Milestone 2.12A).
-- Nullable, like home_*/work_* - most rows (every individual Civilian/
-- Guard/Merchant agent) leave these unset; population IS NULL is the
-- presence check for the whole group (population = 0 is a legitimate
-- empty-group state, distinct from "no CreatureGroupState at all" - the
-- same NULL-vs-zero distinction home_map_id already relies on).
--
-- No new table: a CreatureGroup agent reuses the exact same ai_agents
-- (map_id, spawn_id) identity AgentRegistry/AgentPersistence already give
-- every other AgentType - see AgentRecord.h's own comment on what SpawnId
-- means for this AgentType. spawn_id for a CreatureGroup row must be
-- chosen from a range reserved well above any real creature.guid in the
-- world DB (e.g. >= 1,000,000,000) so
-- Map::GetCreatureBySpawnId(spawn_id) can never accidentally resolve an
-- unrelated real creature spawn - there is no column-level constraint for
-- this, it is an admin/seed-time convention, the same trust boundary
-- HomeLocation/WorkLocation coordinates already rely on not being
-- validated against real terrain either.
--

ALTER TABLE `ai_agents`
    ADD COLUMN `population` INT UNSIGNED NULL DEFAULT NULL AFTER `economy_version`,
    ADD COLUMN `territory_x` FLOAT NULL DEFAULT NULL AFTER `population`,
    ADD COLUMN `territory_y` FLOAT NULL DEFAULT NULL AFTER `territory_x`,
    ADD COLUMN `territory_z` FLOAT NULL DEFAULT NULL AFTER `territory_y`;

--
-- Test group (spawn_id 1,000,000,001, well inside the reserved range
-- above): the first persistent CreatureGroup, purely to make 2.12A's own
-- runtime gate ("restart preserves the same AgentId and group state")
-- concretely testable. Population/territory are placeholder values, not
-- tied to any real spawn - a CreatureGroup never binds to a live
-- Creature, so there is no terrain/spawn validity requirement on them.
-- Idempotent against the existing uq_ai_agents_world_binding (map_id,
-- spawn_id) key, the same convention every other seed row in this table
-- already follows.
--

INSERT INTO `ai_agents`
    (`agent_type`, `map_id`, `spawn_id`, `population`, `territory_x`, `territory_y`, `territory_z`)
VALUES
    (3, 0, 1000000001, 6, 0.0, 0.0, 0.0)
ON DUPLICATE KEY UPDATE
    `agent_type` = VALUES(`agent_type`),
    `population` = VALUES(`population`),
    `territory_x` = VALUES(`territory_x`),
    `territory_y` = VALUES(`territory_y`),
    `territory_z` = VALUES(`territory_z`);
