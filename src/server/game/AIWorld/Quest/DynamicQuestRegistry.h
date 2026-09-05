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
#include "ObjectGuid.h"

#include <map>
#include <vector>

struct QuestProposal;

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
// stays entirely in DynamicQuestLifecycle.h. Find() is deliberately
// const-only (Milestone 2.13C2 P2 fix, round 1, STATIC review: an
// earlier version's mutable overload let a caller assign straight into
// State/Progress/etc., completely bypassing every transition invariant).
//
// Milestone 2.13C2 P2 fix, round 3 (STATIC review): Accept()/
// ApplyProgress()/Complete()/Fail()/Expire() below are the ONLY way a
// stored instance may change after Offer() - there is deliberately no
// public "commit an already-computed DynamicQuestTransitionResult"
// entry point. An earlier version had one (ApplyTransition(), even
// guarded by an optimistic-concurrency revision check) - but
// DynamicQuestInstance is a plain public value type, so any caller can
// construct one with an arbitrary State/Progress/etc, give it the SAME
// Id and Revision a real stored instance currently has, and legitimately
// produce an IsAccepted() DynamicQuestTransitionResult from it via one
// of DynamicQuestLifecycle.h's own pure functions - a revision check
// alone cannot tell that fabricated result apart from one computed from
// this registry's own authoritative stored value, because both are
// externally reproducible. The methods below close that gap by
// construction instead of by proof: each one looks up its OWN current
// stored instance internally and is the only thing that ever passes it
// into a DynamicQuestLifecycle transition function - a caller supplies
// only primitive arguments (the id and whatever the transition itself
// needs), never a DynamicQuestInstance/DynamicQuestTransitionResult of
// its own construction. Read-decide-write happens within a single call,
// so nothing else can observe or act on this registry in between (this
// class is world-thread-only, like every other AIWorld registry) - a
// stale-commit race is therefore impossible by construction, not merely
// checked for.
//
// Milestone 2.13C2 P2 fix (STATIC review): _quests is an ordered
// std::map, not std::unordered_map - GetIdsAfterUntil() needs a stable,
// DynamicQuestId-ascending iteration order it can resume from an
// arbitrary cursor via upper_bound(), the same tradeoff (and reasoning)
// AgentGroupRegistry::_groups already makes. Exists because this
// registry has no size limit of its own: nothing in 2.13C2 itself
// removes an Offered instance nobody ever accepts before its own
// ExpiresAtMs - AIWorldMgr::RunDynamicQuestMaintenance() is what keeps
// it bounded in the common case, using GetHighestId()/GetIdsAfterUntil()
// for the same provably-finite, cursor-resumable scan-cycle shape
// AgentGroupRegistry::GetGroupsAfterUntil() already established (see
// that method's own comment for why an `until` snapshot is required to
// avoid starving the earliest-created entries under continuous
// creation). Milestone 2.13C2 P3 fix (STATIC review): a creation rate
// that outpaces that reclamation is additionally capped by
// AIWorld.DynamicQuestMaxLive - see AIWorldMgr::CreateDynamicQuestOffer()
// and _dynamicQuestMaxLive's own comments; that hard backstop lives at
// the AIWorldMgr level (the only place a NEW instance is ever offered
// from), not in this class.
class TC_GAME_API DynamicQuestRegistry
{
    public:
        // Milestone 2.13C2 P3 fix (STATIC review): the ONLY way a NEW
        // instance ever enters this registry - internally calls the pure
        // OfferDynamicQuest() itself (never accepting an already-
        // constructed DynamicQuestInstance from the caller) and stores
        // the result only if that succeeds, so a freshly-stored entry is
        // always guaranteed by construction to be a genuine, just-minted
        // Offered instance (State==Offered, Progress==0, no
        // AcceptedByPlayerGuid, empty ConsumedProgressEventIds) - never
        // an arbitrary caller-supplied value in some other state. An
        // earlier version instead took a public Add(DynamicQuestInstance)
        // that happily stored any well-formed value object a caller
        // passed in, including an already-Active or already-Completed
        // one. Rejects InvalidQuestId (id == DynamicQuestId{0}, from
        // OfferDynamicQuest() itself) and DuplicateQuestId (id already
        // registered - defense in depth only, unreachable given a
        // correct monotonic allocator).
        DynamicQuestTransitionResult Offer(DynamicQuestId id, QuestProposal const& proposal, uint64 nowMs);

        DynamicQuestInstance const* Find(DynamicQuestId id) const;

        // Milestone 2.13C2 P2 fix, round 3 (STATIC review): Offered ->
        // Active - looks up id's own CURRENTLY stored instance internally
        // and passes THAT (never anything a caller supplies) to
        // AcceptDynamicQuest(), committing the result only if accepted.
        // Rejects QuestNotFound (no stored instance with this id) in
        // addition to every reason AcceptDynamicQuest() itself can
        // return - see that function's own comment.
        DynamicQuestTransitionResult Accept(DynamicQuestId id, ObjectGuid playerGuid, uint64 nowMs);

        // Same shape as Accept(), for ApplyDynamicQuestProgress().
        DynamicQuestTransitionResult ApplyProgress(DynamicQuestId id, ObjectGuid playerGuid, uint64 progressEventId, uint64 nowMs);

        // Same shape as Accept(), for CompleteDynamicQuest().
        DynamicQuestTransitionResult Complete(DynamicQuestId id, uint64 nowMs);

        // Same shape as Accept(), for FailDynamicQuest().
        DynamicQuestTransitionResult Fail(DynamicQuestId id, uint64 nowMs);

        // Same shape as Accept(), for ExpireDynamicQuest(). Does not
        // itself remove the instance once Expired - see
        // AIWorldMgr::RunDynamicQuestMaintenance()'s own comment for why
        // "expire" and "reclaim the now-terminal entry" stay two
        // separate, deliberate steps rather than one method doing both.
        DynamicQuestTransitionResult Expire(DynamicQuestId id, uint64 nowMs);

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
