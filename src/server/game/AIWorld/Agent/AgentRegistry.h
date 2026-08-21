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

// Owns every persistent agent's AgentRecord. An agent registered here keeps
// existing (as an Abstract record) across its Creature being unloaded and
// reloaded - only BindCreature()/UnbindCreature() calls change that, never
// the Creature's own lifecycle directly. Deliberately holds no Creature*/
// Map* anywhere, only SpawnId/MapId/RuntimeGuid.
//
// Not persisted yet (Milestone 2.2): AgentId is only stable for this
// process's lifetime. Not thread-safe: like AIWorldMgr itself, only ever
// touched from the world update thread.
class TC_GAME_API AgentRegistry
{
    public:
        // Idempotent: registering the same map/spawn twice returns the
        // existing AgentId instead of creating a second agent for it.
        AgentId RegisterCreatureAgent(AgentType type, uint32 mapId, uint64 spawnId);

        AgentRecord* Find(AgentId id);
        AgentRecord* FindBySpawn(uint32 mapId, uint64 spawnId);

        void BindCreature(AgentId id, Creature const& creature);
        void UnbindCreature(AgentId id);

        std::vector<AgentId> GetAgents() const;

    private:
        uint64 _nextAgentId = 1;
        std::unordered_map<uint64, AgentRecord> _agents;
};

#endif // AIWORLD_AGENTREGISTRY_H
