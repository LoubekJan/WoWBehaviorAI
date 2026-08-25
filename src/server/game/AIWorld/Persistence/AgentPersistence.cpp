/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AgentPersistence.h"
#include "Agent/AgentLocation.h"
#include "Agent/AgentRegistry.h"
#include "DatabaseEnv.h"
#include "Log.h"

uint32 AgentPersistence::LoadAgents(AgentRegistry& registry)
{
    uint32 loaded = 0;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENTS);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        TC_LOG_INFO("ai.world", "AI persistence loaded 0 agents");
        return 0;
    }

    do
    {
        Field* fields = result->Fetch();

        AgentRecord record;
        record.Id = AgentId{ fields[0].GetUInt64() };
        record.Type = AgentType(fields[1].GetUInt8());
        record.MapId = fields[2].GetUInt32();
        record.SpawnId = fields[3].GetUInt64();
        record.RuntimeGuid = ObjectGuid::Empty;
        record.WorldState = AgentWorldState::Abstract;
        record.SnapshotSequence = 0;

        // Milestone 2.11A: home_map_id/work_map_id are the presence check
        // for the whole pair - always NULL or set together, never partial
        // (see ai_agents' own migration comment).
        if (!fields[4].IsNull())
        {
            AgentLocation home;
            home.MapId = fields[4].GetUInt32();
            home.X = fields[5].GetFloat();
            home.Y = fields[6].GetFloat();
            home.Z = fields[7].GetFloat();
            home.Orientation = fields[8].GetFloat();
            record.HomeLocation = home;
        }

        if (!fields[9].IsNull())
        {
            AgentLocation work;
            work.MapId = fields[9].GetUInt32();
            work.X = fields[10].GetFloat();
            work.Y = fields[11].GetFloat();
            work.Z = fields[12].GetFloat();
            work.Orientation = fields[13].GetFloat();
            record.WorkLocation = work;
        }

        // Milestone 2.11E2/2.11E2 P2/P3 fix: NOT NULL columns (default 0) -
        // no presence check needed, unlike home_*/work_* above. money is
        // BIGINT UNSIGNED (GetUInt64), not INT UNSIGNED like food/resource
        // - see AgentEconomyState::Money.
        record.EconomyState.Money = fields[14].GetUInt64();
        record.EconomyState.Food = fields[15].GetUInt32();
        record.EconomyState.Resource = fields[16].GetUInt32();
        record.EconomyState.LastRewardedWorkWindowId = fields[17].GetUInt64();
        record.EconomyState.Version = fields[18].GetUInt64();

        if (!registry.Add(record))
            continue;

        TC_LOG_INFO("ai.world", "AI agent loaded id={} type={} map={} spawn={} state=ABSTRACT home={} work={} money={} food={} resource={} lastRewardedWorkWindowId={} economyVersion={}",
            record.Id.Value, ToString(record.Type), record.MapId, record.SpawnId,
            record.HomeLocation.has_value(), record.WorkLocation.has_value(),
            record.EconomyState.Money, record.EconomyState.Food, record.EconomyState.Resource,
            record.EconomyState.LastRewardedWorkWindowId, record.EconomyState.Version);

        ++loaded;
    } while (result->NextRow());

    TC_LOG_INFO("ai.world", "AI persistence loaded {} agents", loaded);
    return loaded;
}

AgentId AgentPersistence::FindBinding(uint32 mapId, uint64 spawnId)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_AI_AGENT_BY_BINDING);
    stmt->setUInt32(0, mapId);
    stmt->setUInt64(1, spawnId);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
        return AgentId{};

    Field* fields = result->Fetch();
    return AgentId{ fields[0].GetUInt64() };
}

AgentId AgentPersistence::CreateCreatureAgent(AgentType type, uint32 mapId, uint64 spawnId)
{
    if (AgentId existingId = FindBinding(mapId, spawnId))
    {
        TC_LOG_WARN("ai.world", "AgentPersistence: map={} spawn={} already exists in ai_agents as agent id={}, reusing it instead of inserting a duplicate",
            mapId, spawnId, existingId.Value);
        return existingId;
    }

    CharacterDatabasePreparedStatement* insertStmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_AI_AGENT);
    insertStmt->setUInt8(0, uint8(type));
    insertStmt->setUInt32(1, mapId);
    insertStmt->setUInt64(2, spawnId);
    CharacterDatabase.DirectExecute(insertStmt);

    // DirectExecute() doesn't report success/failure, and agent_id is
    // MySQL-assigned (AUTO_INCREMENT) rather than chosen here, so the only
    // way to know what id - or whether one at all - actually got stored is
    // to read it back by the unique (map_id, spawn_id) binding.
    AgentId newId = FindBinding(mapId, spawnId);
    if (!newId)
        TC_LOG_ERROR("ai.world", "AgentPersistence: INSERT for map={} spawn={} did not produce a readable row, agent was not created", mapId, spawnId);

    return newId;
}

void AgentPersistence::SaveEconomyState(AgentId id, AgentEconomyState& state)
{
    // 2.11E2 P3 fix: unconditional, first thing, regardless of what the
    // caller already did - see this method's own header comment for why
    // the bump lives here rather than being trusted to every caller.
    ++state.Version;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AI_AGENT_ECONOMY);
    stmt->setUInt64(0, state.Money);
    stmt->setUInt32(1, state.Food);
    stmt->setUInt32(2, state.Resource);
    stmt->setUInt64(3, state.LastRewardedWorkWindowId);
    stmt->setUInt64(4, state.Version);
    stmt->setUInt64(5, id.Value);

    // Milestone 2.11E2 P3 fix: bound again for the statement's own
    // "AND economy_version < ?" guard - see AgentEconomyState::Version and
    // CHAR_UPD_AI_AGENT_ECONOMY's own comment for why.
    stmt->setUInt64(6, state.Version);

    // Fire-and-forget by design - see the class comment. The world update
    // thread must never wait on this.
    CharacterDatabase.Execute(stmt);
}
