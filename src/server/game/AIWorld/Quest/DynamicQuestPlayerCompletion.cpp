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

#include "DynamicQuestPlayerCompletion.h"
#include "DynamicQuestInstance.h"
#include "DynamicQuestLifecycle.h"

#include <cmath>

char const* ToString(DynamicQuestPlayerCompleteReason reason)
{
    switch (reason)
    {
        case DynamicQuestPlayerCompleteReason::NotAttempted:           return "NOT_ATTEMPTED";
        case DynamicQuestPlayerCompleteReason::None:                   return "NONE";
        case DynamicQuestPlayerCompleteReason::QuestNotFound:          return "QUEST_NOT_FOUND";
        case DynamicQuestPlayerCompleteReason::PlayerInvalid:          return "PLAYER_INVALID";
        case DynamicQuestPlayerCompleteReason::PlayerMismatch:         return "PLAYER_MISMATCH";
        case DynamicQuestPlayerCompleteReason::GiverMissing:           return "GIVER_MISSING";
        case DynamicQuestPlayerCompleteReason::GiverChanged:           return "GIVER_CHANGED";
        case DynamicQuestPlayerCompleteReason::GiverUnavailable:       return "GIVER_UNAVAILABLE";
        case DynamicQuestPlayerCompleteReason::InteractionRangeInvalid: return "INTERACTION_RANGE_INVALID";
        case DynamicQuestPlayerCompleteReason::OutOfRange:             return "OUT_OF_RANGE";
        case DynamicQuestPlayerCompleteReason::ProgressIncomplete:     return "PROGRESS_INCOMPLETE";
        case DynamicQuestPlayerCompleteReason::RewardMoneyLimit:       return "REWARD_MONEY_LIMIT";
        case DynamicQuestPlayerCompleteReason::CompleteRejected:       return "COMPLETE_REJECTED";
    }
    return "UNKNOWN";
}

DynamicQuestPlayerCompleteReason CheckDynamicQuestPlayerCompleteApplicability(
    DynamicQuestInstance const& instance,
    ObjectGuid playerGuid,
    DynamicQuestPlayerCompleteFacts const& player,
    DynamicQuestGiverCompleteFacts const& giver,
    float playerToGiverDistanceYards,
    float maxInteractionRangeYards,
    uint32 maxMoneyAmount)
{
    if (!player.IsPlayerGuid || !player.Resolved || !player.Alive)
        return DynamicQuestPlayerCompleteReason::PlayerInvalid;

    if (playerGuid != instance.AcceptedByPlayerGuid)
        return DynamicQuestPlayerCompleteReason::PlayerMismatch;

    if (!giver.RecordExists)
        return DynamicQuestPlayerCompleteReason::GiverMissing;

    if (giver.RuntimeGuid != instance.GiverRuntimeGuid)
        return DynamicQuestPlayerCompleteReason::GiverChanged;

    if (!giver.Materialized || !giver.AIWorldControlled || !giver.Alive)
        return DynamicQuestPlayerCompleteReason::GiverUnavailable;

    if (player.MapId != giver.MapId)
        return DynamicQuestPlayerCompleteReason::OutOfRange;

    // See 2.13C3's own InteractionRangeInvalid comment
    // (DynamicQuestPlayerAcceptance.cpp) for why this is checked before
    // ever consulting playerToGiverDistanceYards.
    if (!std::isfinite(maxInteractionRangeYards) || maxInteractionRangeYards < 1.0f)
        return DynamicQuestPlayerCompleteReason::InteractionRangeInvalid;

    if (!std::isfinite(playerToGiverDistanceYards) ||
        playerToGiverDistanceYards < 0.0f ||
        playerToGiverDistanceYards > maxInteractionRangeYards)
        return DynamicQuestPlayerCompleteReason::OutOfRange;

    // Same canonical rule CompleteDynamicQuest() itself consults (see
    // IsDynamicQuestObjectiveComplete()'s own comment in
    // DynamicQuestLifecycle.h) - "is this quest done" is never answered
    // two different ways in two different places.
    if (!IsDynamicQuestObjectiveComplete(instance))
        return DynamicQuestPlayerCompleteReason::ProgressIncomplete;

    // 64-bit arithmetic so this can never itself underflow/wrap into a
    // false negative, regardless of how close player.Money already is to
    // maxMoneyAmount.
    if (uint64(player.Money) + uint64(instance.RewardMoneyCopper) > uint64(maxMoneyAmount))
        return DynamicQuestPlayerCompleteReason::RewardMoneyLimit;

    return DynamicQuestPlayerCompleteReason::None;
}
