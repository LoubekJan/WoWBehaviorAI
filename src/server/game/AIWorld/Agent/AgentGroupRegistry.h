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

#ifndef AIWORLD_AGENTGROUPREGISTRY_H
#define AIWORLD_AGENTGROUPREGISTRY_H

#include "AgentGroupRecord.h"
#include "Define.h"
#include "GroupId.h"
#include <unordered_map>
#include <vector>

// Milestone 2.12D (STATIC review P2 fix): owns every persistent AgentGroup's
// AgentGroupRecord for the process's lifetime - the group-side counterpart
// to AgentRegistry, deliberately its own class rather than another
// AgentType inside AgentRegistry. A group never binds to a live Creature
// and has no SpawnId/MapId identity of its own (see GroupId.h), so it has
// none of AgentRegistry's Creature-binding surface (BindCreature/
// UnbindCreature/FindBySpawn) - only identity storage and lookup.
//
// Purely in-memory: does not generate GroupIds and does not talk to any
// database. AgentGroupPersistence is the authority for minting and loading
// GroupIds - this class only ever receives already-assigned ones through
// Add(). Not thread-safe in general: mutating calls are world-thread-only,
// like AgentRegistry/AIWorldMgr itself.
class TC_GAME_API AgentGroupRegistry
{
    public:
        // Adds an already-identified record (from AgentGroupPersistence).
        // Rejects and returns false for GroupId=0 or a duplicate GroupId.
        bool Add(AgentGroupRecord record);

        AgentGroupRecord* Find(GroupId id);
        AgentGroupRecord const* Find(GroupId id) const;

        // Milestone 2.12E1: erases the whole record - membership included,
        // since it lives inside AgentGroupRecord::Members, not a separate
        // container. Only ever called by AgentGroupLifecycleSystem::
        // DissolveGroup(), after AgentGroupPersistence::DeleteGroup() has
        // already removed the DB-side rows. Returns whether a record was
        // actually erased (false for an unknown GroupId - not an error,
        // the caller decides what that means).
        bool Remove(GroupId id);

        std::vector<GroupId> GetGroups() const;

    private:
        std::unordered_map<uint64, AgentGroupRecord> _groups;
};

#endif // AIWORLD_AGENTGROUPREGISTRY_H
