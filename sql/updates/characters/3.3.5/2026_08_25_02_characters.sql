--
-- AIWorld: WORK reward idempotency key (Milestone 2.11E2 P2 fix). Without
-- this, a WORK reward's only identity was AgentRecord::RoutineActivityState
-- - runtime-only, cleared on every dematerialize/restart - so a
-- rematerialize or restart that landed back inside the same synthetic work
-- window would pay the reward again. Persisted alongside money/food/
-- resource so both change together in one UPDATE, never two separate
-- async writes that could land only one of them.
--

ALTER TABLE `ai_agents`
    ADD COLUMN `last_rewarded_work_window_id` BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER `resource`;
