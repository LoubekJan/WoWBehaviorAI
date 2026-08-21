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

        if (!registry.Add(record))
            continue;

        TC_LOG_INFO("ai.world", "AI agent loaded id={} type={} map={} spawn={} state=ABSTRACT",
            record.Id.Value, ToString(record.Type), record.MapId, record.SpawnId);

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
