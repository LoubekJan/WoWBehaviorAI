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

#ifndef AIWORLD_DYNAMICQUESTREGISTRY_H
#define AIWORLD_DYNAMICQUESTREGISTRY_H

#include "Define.h"
#include "DynamicQuestId.h"
#include "DynamicQuestInstance.h"

#include <unordered_map>

// Milestone 2.13C2: owns every currently-live DynamicQuestInstance for the
// process's lifetime - the quest-lifecycle counterpart to
// AgentGroupRegistry/AgentRegistry. Purely in-memory: does not mint
// DynamicQuestIds itself (AIWorldMgr::AllocateDynamicQuestId() /
// AdvanceDynamicQuestIdCounter() is the sole authority for that - see
// their own comments) and does not talk to any database. Not thread-safe
// in general: mutating calls are world-thread-only, like every other
// AIWorld registry.
//
// Holds no lifecycle logic of its own - every transition stays entirely
// in DynamicQuestLifecycle.h. A caller reads an instance out via Find(),
// calls a DynamicQuestLifecycle transition function to get a NEW value,
// and writes that new value back in over the old one; this class has no
// "Update" method that would let a caller bypass those pure transition
// functions.
class TC_GAME_API DynamicQuestRegistry
{
    public:
        // Adds an already-identified instance (from
        // AIWorldMgr::AllocateDynamicQuestId() + OfferDynamicQuest()).
        // Rejects and returns false for DynamicQuestId{0} or a duplicate
        // id - never overwrites an existing instance.
        bool Add(DynamicQuestInstance instance);

        DynamicQuestInstance* Find(DynamicQuestId id);
        DynamicQuestInstance const* Find(DynamicQuestId id) const;

        // Returns whether an instance was actually erased - false for an
        // unknown id, not an error.
        bool Remove(DynamicQuestId id);

        uint32 GetCount() const;

    private:
        std::unordered_map<uint64, DynamicQuestInstance> _quests;
};

#endif // AIWORLD_DYNAMICQUESTREGISTRY_H
