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

#include "AgentRegistry.h"
#include "Creature.h"
#include "Log.h"

AgentId AgentRegistry::RegisterCreatureAgent(AgentType type, uint32 mapId, uint64 spawnId)
{
    if (AgentRecord* existing = FindBySpawn(mapId, spawnId))
    {
        TC_LOG_WARN("ai.world", "AgentRegistry: map={} spawn={} is already registered as agent id={}, ignoring duplicate registration",
            mapId, spawnId, existing->Id.Value);
        return existing->Id;
    }

    AgentId id{ _nextAgentId++ };

    AgentRecord record;
    record.Id = id;
    record.Type = type;
    record.MapId = mapId;
    record.SpawnId = spawnId;
    record.WorldState = AgentWorldState::Abstract;

    _agents.emplace(id.Value, record);

    TC_LOG_INFO("ai.world", "AI agent registered id={} type={} map={} spawn={} state=ABSTRACT",
        id.Value, ToString(type), mapId, spawnId);

    return id;
}

AgentRecord* AgentRegistry::Find(AgentId id)
{
    auto it = _agents.find(id.Value);
    return it != _agents.end() ? &it->second : nullptr;
}

AgentRecord* AgentRegistry::FindBySpawn(uint32 mapId, uint64 spawnId)
{
    for (auto& entry : _agents)
    {
        if (entry.second.MapId == mapId && entry.second.SpawnId == spawnId)
            return &entry.second;
    }
    return nullptr;
}

void AgentRegistry::BindCreature(AgentId id, Creature const& creature)
{
    AgentRecord* record = Find(id);
    if (!record)
        return;

    record->RuntimeGuid = creature.GetGUID();
    record->WorldState = AgentWorldState::Materialized;

    TC_LOG_INFO("ai.world", "AI agent id={} materialized spawn={} guid={}",
        id.Value, record->SpawnId, record->RuntimeGuid.ToString());
}

void AgentRegistry::UnbindCreature(AgentId id)
{
    AgentRecord* record = Find(id);
    if (!record)
        return;

    TC_LOG_INFO("ai.world", "AI agent id={} dematerialized spawn={}", id.Value, record->SpawnId);

    record->RuntimeGuid = ObjectGuid::Empty;
    record->WorldState = AgentWorldState::Abstract;
}

std::vector<AgentId> AgentRegistry::GetAgents() const
{
    std::vector<AgentId> ids;
    ids.reserve(_agents.size());
    for (auto const& entry : _agents)
        ids.push_back(entry.second.Id);
    return ids;
}
