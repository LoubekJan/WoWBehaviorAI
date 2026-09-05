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

#include "DynamicQuestLifecycle.h"
#include "Inference/QuestProposal.h"

#include <algorithm>
#include <limits>
#include <utility>

char const* ToString(DynamicQuestState state)
{
    switch (state)
    {
        case DynamicQuestState::Offered:   return "OFFERED";
        case DynamicQuestState::Active:    return "ACTIVE";
        case DynamicQuestState::Completed: return "COMPLETED";
        case DynamicQuestState::Failed:    return "FAILED";
        case DynamicQuestState::Expired:   return "EXPIRED";
    }
    return "UNKNOWN";
}

char const* ToString(DynamicQuestRejectReason reason)
{
    switch (reason)
    {
        case DynamicQuestRejectReason::NotAttempted:           return "NOT_ATTEMPTED";
        case DynamicQuestRejectReason::None:                   return "NONE";
        case DynamicQuestRejectReason::AlreadyTerminal:        return "ALREADY_TERMINAL";
        case DynamicQuestRejectReason::InvalidTransition:      return "INVALID_TRANSITION";
        case DynamicQuestRejectReason::InvalidQuestId:         return "INVALID_QUEST_ID";
        case DynamicQuestRejectReason::InvalidPlayer:          return "INVALID_PLAYER";
        case DynamicQuestRejectReason::PlayerMismatch:         return "PLAYER_MISMATCH";
        case DynamicQuestRejectReason::AlreadyExpired:         return "ALREADY_EXPIRED";
        case DynamicQuestRejectReason::NotYetExpired:          return "NOT_YET_EXPIRED";
        case DynamicQuestRejectReason::ProgressIncomplete:     return "PROGRESS_INCOMPLETE";
        case DynamicQuestRejectReason::ProgressAlreadyComplete: return "PROGRESS_ALREADY_COMPLETE";
        case DynamicQuestRejectReason::InvalidProgressEvent:   return "INVALID_PROGRESS_EVENT";
        case DynamicQuestRejectReason::DuplicateProgressEvent: return "DUPLICATE_PROGRESS_EVENT";
    }
    return "UNKNOWN";
}

namespace
{
    bool IsTerminal(DynamicQuestState state)
    {
        return state == DynamicQuestState::Completed ||
               state == DynamicQuestState::Failed ||
               state == DynamicQuestState::Expired;
    }

    // Never wraps around, regardless of how large nowMs or expiryMs are.
    uint64 SaturatingAddMs(uint64 nowMs, uint32 expiryMs)
    {
        uint64 remaining = std::numeric_limits<uint64>::max() - nowMs;
        if (uint64(expiryMs) > remaining)
            return std::numeric_limits<uint64>::max();
        return nowMs + expiryMs;
    }

    DynamicQuestTransitionResult Reject(DynamicQuestRejectReason reason)
    {
        DynamicQuestTransitionResult result;
        result.Reason = reason;
        return result;
    }

    // For OfferDynamicQuest() only - a creation, not a transition FROM a
    // prior stored value, so it deliberately does not touch
    // DynamicQuestTransitionResult::SourceRevision or bump anything: the
    // produced instance keeps whatever Revision it was already
    // constructed with (0, its own default).
    DynamicQuestTransitionResult Created(DynamicQuestInstance instance)
    {
        DynamicQuestTransitionResult result;
        result.Reason = DynamicQuestRejectReason::None;
        result.Instance = std::move(instance);
        return result;
    }

    // Milestone 2.13C2 P2 fix (STATIC review): every actual transition
    // (as opposed to OfferDynamicQuest()'s own Created()) goes through
    // here so SourceRevision is always the exact Revision of the
    // instance the transition was computed from, and the produced
    // `next` always ends up at sourceRevision + 1 - see
    // DynamicQuestTransitionResult's own comment for why
    // DynamicQuestRegistry::ApplyTransition() needs both of those to
    // detect a stale commit.
    DynamicQuestTransitionResult Accepted(DynamicQuestInstance next, uint64 sourceRevision)
    {
        DynamicQuestTransitionResult result;
        result.Reason = DynamicQuestRejectReason::None;
        result.SourceRevision = sourceRevision;
        next.Revision = sourceRevision + 1;
        result.Instance = std::move(next);
        return result;
    }
}

DynamicQuestTransitionResult OfferDynamicQuest(DynamicQuestId id, QuestProposal const& proposal, uint64 nowMs)
{
    if (!id)
        return Reject(DynamicQuestRejectReason::InvalidQuestId);

    DynamicQuestInstance instance;
    instance.Id = id;
    instance.State = DynamicQuestState::Offered;

    instance.Giver = proposal.Giver;
    instance.GiverRuntimeGuid = proposal.GiverRuntimeGuid;

    instance.Objective = proposal.Objective;
    instance.TargetGuid = proposal.TargetGuid;
    instance.TargetEntry = proposal.TargetEntry;
    instance.TargetMapId = proposal.TargetMapId;

    instance.RequiredCount = proposal.RequiredCount;
    instance.Progress = 0;

    instance.CreatedAtMs = nowMs;
    instance.ExpiresAtMs = SaturatingAddMs(nowMs, proposal.ExpiryMs);

    return Created(std::move(instance));
}

bool IsDynamicQuestExpired(DynamicQuestInstance const& instance, uint64 nowMs)
{
    return nowMs >= instance.ExpiresAtMs;
}

DynamicQuestTransitionResult AcceptDynamicQuest(DynamicQuestInstance const& instance, ObjectGuid playerGuid, uint64 nowMs)
{
    if (IsTerminal(instance.State))
        return Reject(DynamicQuestRejectReason::AlreadyTerminal);

    if (instance.State != DynamicQuestState::Offered)
        return Reject(DynamicQuestRejectReason::InvalidTransition);

    if (!playerGuid.IsPlayer())
        return Reject(DynamicQuestRejectReason::InvalidPlayer);

    if (IsDynamicQuestExpired(instance, nowMs))
        return Reject(DynamicQuestRejectReason::AlreadyExpired);

    DynamicQuestInstance next = instance;
    next.State = DynamicQuestState::Active;
    next.AcceptedByPlayerGuid = playerGuid;
    return Accepted(std::move(next), instance.Revision);
}

DynamicQuestTransitionResult ApplyDynamicQuestProgress(DynamicQuestInstance const& instance, ObjectGuid playerGuid, uint64 progressEventId, uint64 nowMs)
{
    if (IsTerminal(instance.State))
        return Reject(DynamicQuestRejectReason::AlreadyTerminal);

    if (instance.State != DynamicQuestState::Active)
        return Reject(DynamicQuestRejectReason::InvalidTransition);

    if (IsDynamicQuestExpired(instance, nowMs))
        return Reject(DynamicQuestRejectReason::AlreadyExpired);

    if (progressEventId == 0)
        return Reject(DynamicQuestRejectReason::InvalidProgressEvent);

    if (playerGuid != instance.AcceptedByPlayerGuid)
        return Reject(DynamicQuestRejectReason::PlayerMismatch);

    bool alreadyConsumed = std::find(
        instance.ConsumedProgressEventIds.begin(),
        instance.ConsumedProgressEventIds.end(),
        progressEventId) != instance.ConsumedProgressEventIds.end();
    if (alreadyConsumed)
        return Reject(DynamicQuestRejectReason::DuplicateProgressEvent);

    // Checked AFTER the duplicate check above so a replay of an already-
    // counted event is always reported as DuplicateProgressEvent, never
    // masked by saturation. A genuinely new event is never stored once
    // already saturated - see ConsumedProgressEventIds' own comment for
    // why this keeps both its size and this function's own duplicate
    // check bounded by RequiredCount.
    if (instance.Progress >= instance.RequiredCount)
        return Reject(DynamicQuestRejectReason::ProgressAlreadyComplete);

    DynamicQuestInstance next = instance;
    next.ConsumedProgressEventIds.push_back(progressEventId);
    ++next.Progress;
    return Accepted(std::move(next), instance.Revision);
}

DynamicQuestTransitionResult CompleteDynamicQuest(DynamicQuestInstance const& instance, uint64 nowMs)
{
    if (IsTerminal(instance.State))
        return Reject(DynamicQuestRejectReason::AlreadyTerminal);

    if (instance.State != DynamicQuestState::Active)
        return Reject(DynamicQuestRejectReason::InvalidTransition);

    if (IsDynamicQuestExpired(instance, nowMs))
        return Reject(DynamicQuestRejectReason::AlreadyExpired);

    if (instance.Progress < instance.RequiredCount)
        return Reject(DynamicQuestRejectReason::ProgressIncomplete);

    DynamicQuestInstance next = instance;
    next.State = DynamicQuestState::Completed;
    return Accepted(std::move(next), instance.Revision);
}

DynamicQuestTransitionResult FailDynamicQuest(DynamicQuestInstance const& instance, uint64 nowMs)
{
    if (IsTerminal(instance.State))
        return Reject(DynamicQuestRejectReason::AlreadyTerminal);

    if (instance.State != DynamicQuestState::Active)
        return Reject(DynamicQuestRejectReason::InvalidTransition);

    if (IsDynamicQuestExpired(instance, nowMs))
        return Reject(DynamicQuestRejectReason::AlreadyExpired);

    DynamicQuestInstance next = instance;
    next.State = DynamicQuestState::Failed;
    return Accepted(std::move(next), instance.Revision);
}

DynamicQuestTransitionResult ExpireDynamicQuest(DynamicQuestInstance const& instance, uint64 nowMs)
{
    if (IsTerminal(instance.State))
        return Reject(DynamicQuestRejectReason::AlreadyTerminal);

    if (!IsDynamicQuestExpired(instance, nowMs))
        return Reject(DynamicQuestRejectReason::NotYetExpired);

    DynamicQuestInstance next = instance;
    next.State = DynamicQuestState::Expired;
    return Accepted(std::move(next), instance.Revision);
}
