--
-- AIWorld: persistent, monotonic GroupId allocator (Milestone 2.12E1 P2
-- fix, STATIC review on c6dc712ee65e). AgentGroupPersistence previously
-- seeded its in-memory _nextGroupId from MAX(group_id) over CURRENTLY
-- EXISTING ai_agent_groups rows - once a group is dissolved (its row
-- deleted), that id silently drops out of the MAX() and gets handed to
-- the next, entirely unrelated CreateGroup() call after a restart,
-- directly contradicting AgentGroupPersistence's own "never reused once
-- issued" comment. This table is append-only and never deleted from - its
-- one row's next_group_id only ever increases, independent of which
-- ai_agent_groups rows currently exist, so a dissolved group's id can
-- never be recycled.
--

CREATE TABLE IF NOT EXISTS `ai_agent_group_id_sequence` (
    `id` TINYINT UNSIGNED NOT NULL,
    `next_group_id` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AIWorld persistent GroupId allocator (Milestone 2.12E1 P2 fix) - single row, id=1, next_group_id only ever increases';

--
-- Seed the single row from whatever the highest group_id already in
-- ai_agent_groups is (today: 1, the 2.12A/2.12D-migrated test group) - +1
-- so the next CreateGroup() call does not collide with it. COALESCE
-- handles the (here, hypothetical) zero-groups case too, still producing
-- exactly one row.
--

INSERT INTO `ai_agent_group_id_sequence` (`id`, `next_group_id`)
SELECT 1, COALESCE(MAX(`group_id`), 0) + 1 FROM `ai_agent_groups`;
