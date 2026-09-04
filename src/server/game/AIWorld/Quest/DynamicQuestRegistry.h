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
#include "DynamicQuestLifecycle.h"

#include <map>
#include <vector>

// Milestone 2.13C2: owns every currently-live DynamicQuestInstance for the
// process's lifetime - the quest-lifecycle counterpart to
// AgentGroupRegistry/AgentRegistry. Purely in-memory: does not mint
// DynamicQuestIds itself (AIWorldMgr::AllocateDynamicQuestId() /
// AdvanceDynamicQuestIdCounter() is the sole authority for that - see
// their own comments) and does not talk to any database. Not thread-safe
// in general: mutating calls are world-thread-only, like every other
// AIWorld registry.
//
// Holds no lifecycle DECISION logic of its own - every transition rule
// stays entirely in DynamicQuestLifecycle.h. A caller reads an instance
// out via Find(), calls a DynamicQuestLifecycle transition function to
// get a NEW value, and commits it back via ApplyTransition() - never by
// mutating a stored instance directly. Find() is deliberately const-only
// (Milestone 2.13C2 P2 fix, STATIC review: an earlier version's mutable
// overload let a caller assign straight into State/Progress/etc.,
// completely bypassing AcceptDynamicQuest()/ApplyDynamicQuestProgress()/
// expiry/replay-guard invariants).
//
// Milestone 2.13C2 P2 fix (STATIC review): _quests is an ordered
// std::map, not std::unordered_map - GetIdsAfterUntil() needs a stable,
// DynamicQuestId-ascending iteration order it can resume from an
// arbitrary cursor via upper_bound(), the same tradeoff (and reasoning)
// AgentGroupRegistry::_groups already makes. Exists because this
// registry is otherwise unbounded: nothing in 2.13C2 itself removes an
// Offered instance nobody ever accepts before its own ExpiresAtMs -
// AIWorldMgr::RunDynamicQuestMaintenance() is what keeps it bounded,
// using GetHighestId()/GetIdsAfterUntil() for the same provably-finite,
// cursor-resumable scan-cycle shape AgentGroupRegistry::
// GetGroupsAfterUntil() already established (see that method's own
// comment for why an `until` snapshot is required to avoid starving the
// earliest-created entries under continuous creation).
class TC_GAME_API DynamicQuestRegistry
{
    public:
        // Adds an already-identified instance (from
        // AIWorldMgr::AllocateDynamicQuestId() + OfferDynamicQuest()).
        // Rejects and returns false for DynamicQuestId{0} or a duplicate
        // id - never overwrites an existing instance. The only way a NEW
        // id ever enters this registry.
        bool Add(DynamicQuestInstance instance);

        DynamicQuestInstance const* Find(DynamicQuestId id) const;

        // Milestone 2.13C2 P2 fix (STATIC review): the ONLY way an
        // already-stored instance's own fields (State/Progress/
        // AcceptedByPlayerGuid/ConsumedProgressEventIds/...) may change
        // after Add(). Takes an already-computed DynamicQuestTransitionResult
        // from one of DynamicQuestLifecycle.h's pure transition functions
        // (AcceptDynamicQuest()/ApplyDynamicQuestProgress()/
        // CompleteDynamicQuest()/FailDynamicQuest()/ExpireDynamicQuest())
        // - requires result.IsAccepted() and that result.Instance->Id
        // already names a stored instance (Id itself never changes across
        // a transition); returns false and touches nothing otherwise, the
        // same fail-closed shape Add()/Remove() already have.
        bool ApplyTransition(DynamicQuestTransitionResult const& result);

        // Returns whether an instance was actually erased - false for an
        // unknown id, not an error.
        bool Remove(DynamicQuestId id);

        uint32 GetCount() const;

        // Milestone 2.13C2 P2 fix (STATIC review): the highest currently-
        // registered DynamicQuestId, or DynamicQuestId{} (0) if the
        // registry is empty - O(1) via _quests' own ordering (rbegin()).
        // Lets a caller snapshot a maintenance-scan cycle boundary
        // without paying for a full traversal - same pattern as
        // AgentGroupRegistry::GetHighestGroupId().
        DynamicQuestId GetHighestId() const;

        // Milestone 2.13C2 P2 fix (STATIC review): bounded, cursor-based
        // discovery WITHIN one scan cycle - up to maxCount ids strictly
        // greater than after AND less than or equal to until, in
        // ascending DynamicQuestId order, using _quests' own ordering
        // (upper_bound()) rather than materializing every registered id
        // the way a plain "list everything" call would. Returns raw ids
        // only, unfiltered by State/expiry - this registry holds no
        // lifecycle logic of its own, so the caller (AIWorldMgr::
        // RunDynamicQuestMaintenance()) decides what to do with each one.
        // Same shape/reasoning as AgentGroupRegistry::
        // GetGroupsAfterUntil() - see that method's own comment for why
        // `until` is what makes a scan cycle provably finite under
        // continuous quest creation, and why after = DynamicQuestId{} (0)
        // safely starts a cycle from the beginning (every real id is
        // nonzero).
        std::vector<DynamicQuestId> GetIdsAfterUntil(DynamicQuestId after, DynamicQuestId until, uint32 maxCount) const;

    private:
        std::map<uint64, DynamicQuestInstance> _quests;
};

#endif // AIWORLD_DYNAMICQUESTREGISTRY_H
