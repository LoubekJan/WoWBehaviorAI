--
-- AIWorld: TrinityCore-aligned AgentId (Milestone 2.12F4A2, see
-- AIWorld_Current_Roadmap.md's own "2.12F4A2 - TrinityCore-aligned Agent
-- identity" section for the full reasoning). For a persistent non-instance
-- Creature agent, AgentId.Value must equal the TrinityCore Creature
-- SpawnId (world.creature.guid) it is bound to, replacing today's
-- MySQL-assigned AUTO_INCREMENT sequence (1..4), which carries no
-- relationship to the spawn at all. AgentRecord::SpawnId itself is kept -
-- see AgentPersistence.h's own comment for why it still has a distinct
-- meaning (provenance/foreign binding) even once numerically identical to
-- AgentId. ai_agents.agent_id stays AUTO_INCREMENT (MySQL allows an
-- explicit INSERT value into an AUTO_INCREMENT column; the counter simply
-- advances past it, see CHAR_INS_AI_AGENT's own comment) - a future
-- non-creature AgentType that still needs a MySQL-assigned id can keep
-- using it, as long as it never collides with a spawn_id-derived value
-- (that collision policy is a 2.12F4B open point, not resolved here).
--
-- Every ai_agent_group_members.member_agent_id and every
-- ai_long_term_memories agent_id/actor_agent_id/target_agent_id value
-- that names one of these four agents must move together with it in the
-- same migration - an AgentId here is not just a primary key, it is a
-- value copied into every table that references an agent, and this
-- database layer has no FOREIGN KEY constraints to cascade the change
-- (see ai_long_term_memories' and the retired ai_creature_group_members'
-- own comments for why that was a deliberate choice). ai_agent_groups is
-- unaffected: GroupId has been its own domain, separate from AgentId,
-- since the 2026_08_26_00/01 migrations.
--

SET @old_80335  := (SELECT `agent_id` FROM `ai_agents` WHERE `map_id` = 0 AND `spawn_id` = 80335);
SET @old_214023 := (SELECT `agent_id` FROM `ai_agents` WHERE `map_id` = 0 AND `spawn_id` = 214023);
SET @old_214021 := (SELECT `agent_id` FROM `ai_agents` WHERE `map_id` = 0 AND `spawn_id` = 214021);
SET @old_80683  := (SELECT `agent_id` FROM `ai_agents` WHERE `map_id` = 0 AND `spawn_id` = 80683);

--
-- Repoint every reference table first, while the OLD agent_id captured
-- above is still meaningful - ai_agents.agent_id itself only changes
-- after this. Idempotent and safe on a server missing one of these four
-- agents: the corresponding @old_* variable is then NULL, and neither
-- `IN (..., NULL, ...)` nor `WHEN NULL` ever matches a row, so that
-- agent's own branch is simply a no-op instead of an error. Idempotent on
-- re-run too: once a row already holds the new value, re-matching it
-- against the (now-current) @old_* value is again a no-op.
--

UPDATE `ai_agent_group_members`
SET `member_agent_id` = CASE `member_agent_id`
    WHEN @old_80335  THEN 80335
    WHEN @old_214023 THEN 214023
    WHEN @old_214021 THEN 214021
    WHEN @old_80683  THEN 80683
    ELSE `member_agent_id`
END
WHERE `member_agent_id` IN (@old_80335, @old_214023, @old_214021, @old_80683);

UPDATE `ai_long_term_memories`
SET `agent_id` = CASE `agent_id`
    WHEN @old_80335  THEN 80335
    WHEN @old_214023 THEN 214023
    WHEN @old_214021 THEN 214021
    WHEN @old_80683  THEN 80683
    ELSE `agent_id`
END
WHERE `agent_id` IN (@old_80335, @old_214023, @old_214021, @old_80683);

UPDATE `ai_long_term_memories`
SET `actor_agent_id` = CASE `actor_agent_id`
    WHEN @old_80335  THEN 80335
    WHEN @old_214023 THEN 214023
    WHEN @old_214021 THEN 214021
    WHEN @old_80683  THEN 80683
    ELSE `actor_agent_id`
END
WHERE `actor_agent_id` IN (@old_80335, @old_214023, @old_214021, @old_80683);

UPDATE `ai_long_term_memories`
SET `target_agent_id` = CASE `target_agent_id`
    WHEN @old_80335  THEN 80335
    WHEN @old_214023 THEN 214023
    WHEN @old_214021 THEN 214021
    WHEN @old_80683  THEN 80683
    ELSE `target_agent_id`
END
WHERE `target_agent_id` IN (@old_80335, @old_214023, @old_214021, @old_80683);

--
-- Finally ai_agents.agent_id itself (the primary key) - one UPDATE per
-- binding, not a CASE: the new value is simply the spawn_id already named
-- in each row's own WHERE clause, and each WHERE matches at most one row
-- (uq_ai_agents_world_binding), so this cannot collide with itself. A
-- server missing one of these four agents just matches zero rows for
-- that statement.
--

UPDATE `ai_agents` SET `agent_id` = 80335  WHERE `map_id` = 0 AND `spawn_id` = 80335;
UPDATE `ai_agents` SET `agent_id` = 214023 WHERE `map_id` = 0 AND `spawn_id` = 214023;
UPDATE `ai_agents` SET `agent_id` = 214021 WHERE `map_id` = 0 AND `spawn_id` = 214021;
UPDATE `ai_agents` SET `agent_id` = 80683  WHERE `map_id` = 0 AND `spawn_id` = 80683;
