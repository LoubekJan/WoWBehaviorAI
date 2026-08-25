--
-- AIWorld: AgentGroup lifecycle (Milestone 2.12E1). GroupId is no longer
-- MySQL AUTO_INCREMENT - CreateGroup() now mints it explicitly in C++
-- (AgentGroupPersistence's own in-memory counter, seeded at startup from
-- the highest group_id LoadGroups() saw, then advanced by one per
-- CreateGroup() call - the same allocator shape GuildMgr::GenerateGuildId()
-- already uses for guildid, never derived from AgentId/SpawnId/
-- RuntimeGuid). This is what lets CreateGroup() insert the row with an
-- explicit, already-known id and then read it back to confirm the write
-- actually landed before the runtime ever adds it to AgentGroupRegistry -
-- an AUTO_INCREMENT column has no id to bind a WHERE clause to until after
-- the INSERT, which is exactly the "was this id really written" gap this
-- migration closes.
--

ALTER TABLE `ai_agent_groups`
    MODIFY COLUMN `group_id` BIGINT UNSIGNED NOT NULL;
