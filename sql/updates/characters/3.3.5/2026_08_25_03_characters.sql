--
-- AIWorld: monotonic write guard for the economy snapshot (Milestone
-- 2.11E2 P3 fix). AgentPersistence::SaveEconomyState() writes the whole
-- money/food/resource/last_rewarded_work_window_id row through a
-- fire-and-forget async queue, which does not itself guarantee execution
-- order across CharacterDatabase's own async worker threads - without a
-- guard, an older snapshot could in principle finish after a newer one and
-- silently revert it. economy_version is incremented in memory before
-- every write and only ever applied by the UPDATE's own WHERE clause when
-- the stored value is still lower than the one being written.
--

ALTER TABLE `ai_agents`
    ADD COLUMN `economy_version` BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER `last_rewarded_work_window_id`;
