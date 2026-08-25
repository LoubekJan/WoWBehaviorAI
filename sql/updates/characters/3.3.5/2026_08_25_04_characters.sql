--
-- AIWorld: widen money to avoid uint32 wraparound (Milestone 2.11E2 P3
-- fix). Money is the only AgentEconomyState field anything currently
-- mutates (WORK reward, see AgentPersistence.cpp), so it was the only one
-- actually at risk of overflowing back to a small value after enough
-- reward cycles - INT UNSIGNED -> BIGINT UNSIGNED removes that ceiling in
-- practice. A plain MODIFY COLUMN widening; every existing value fits
-- unchanged.
--

ALTER TABLE `ai_agents`
    MODIFY COLUMN `money` BIGINT UNSIGNED NOT NULL DEFAULT 0;
