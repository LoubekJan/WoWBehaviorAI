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

#ifndef AIWORLD_AGENTREGISTRY_H
#define AIWORLD_AGENTREGISTRY_H

#include "AgentId.h"
#include "AgentRecord.h"
#include "AgentType.h"
#include "Define.h"
#include <unordered_map>
#include <vector>

class Creature;

// Owns every persistent agent's AgentRecord for the process's lifetime. An
// agent added here keeps existing (as an Abstract record) across its
// Creature being unloaded and reloaded - only BindCreature()/
// UnbindCreature() calls change that, never the Creature's own lifecycle
// directly. Deliberately holds no Creature*/Map* anywhere, only
// SpawnId/MapId/RuntimeGuid.
//
// Purely in-memory: does not generate AgentIds and does not talk to any
// database. AgentPersistence (Milestone 2.2A) is the authority for minting
// and loading AgentIds - this class only ever receives already-assigned
// ones through Add(). Not thread-safe in general: mutating calls
// (Add/BindCreature/UnbindCreature) are world-thread-only, like AIWorldMgr
// itself. The one documented exception is the const FindBySpawn() overload
// - see AIWorldMgr::OwnsSpawn() for why a read-only lookup through it is
// safe to call from a map-updater thread during grid loading.
class TC_GAME_API AgentRegistry
{
    public:
        // Adds an already-identified record (from AgentPersistence, either
        // freshly created or loaded from ai_agents). Rejects and returns
        // false for AgentId=0, SpawnId=0, a duplicate AgentId, or a
        // duplicate (MapId, SpawnId) binding - these are registry-level
        // invariants, not just persistence-layer ones.
        bool Add(AgentRecord record);

        AgentRecord* Find(AgentId id);
        AgentRecord const* Find(AgentId id) const;
        AgentRecord* FindBySpawn(uint32 mapId, uint64 spawnId);
        AgentRecord const* FindBySpawn(uint32 mapId, uint64 spawnId) const;

        void BindCreature(AgentId id, Creature const& creature);
        void UnbindCreature(AgentId id);

        std::vector<AgentId> GetAgents() const;

    private:
        std::unordered_map<uint64, AgentRecord> _agents;
};

#endif // AIWORLD_AGENTREGISTRY_H
