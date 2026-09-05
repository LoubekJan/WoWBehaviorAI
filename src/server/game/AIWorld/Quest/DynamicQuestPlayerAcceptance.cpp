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

#include "DynamicQuestPlayerAcceptance.h"
#include "DynamicQuestInstance.h"

#include <cmath>

char const* ToString(DynamicQuestPlayerAcceptReason reason)
{
    switch (reason)
    {
        case DynamicQuestPlayerAcceptReason::NotAttempted:    return "NOT_ATTEMPTED";
        case DynamicQuestPlayerAcceptReason::None:            return "NONE";
        case DynamicQuestPlayerAcceptReason::QuestNotFound:   return "QUEST_NOT_FOUND";
        case DynamicQuestPlayerAcceptReason::PlayerInvalid:   return "PLAYER_INVALID";
        case DynamicQuestPlayerAcceptReason::GiverMissing:    return "GIVER_MISSING";
        case DynamicQuestPlayerAcceptReason::GiverChanged:    return "GIVER_CHANGED";
        case DynamicQuestPlayerAcceptReason::GiverUnavailable: return "GIVER_UNAVAILABLE";
        case DynamicQuestPlayerAcceptReason::OutOfRange:      return "OUT_OF_RANGE";
        case DynamicQuestPlayerAcceptReason::InteractionRangeInvalid: return "INTERACTION_RANGE_INVALID";
        case DynamicQuestPlayerAcceptReason::AcceptRejected:  return "ACCEPT_REJECTED";
    }
    return "UNKNOWN";
}

DynamicQuestPlayerAcceptReason CheckDynamicQuestPlayerAcceptApplicability(
    DynamicQuestInstance const& instance,
    DynamicQuestPlayerFacts const& player,
    DynamicQuestGiverAcceptFacts const& giver,
    float playerToGiverDistanceYards,
    float maxInteractionRangeYards)
{
    if (!player.IsPlayerGuid || !player.Resolved || !player.Alive)
        return DynamicQuestPlayerAcceptReason::PlayerInvalid;

    if (!giver.RecordExists)
        return DynamicQuestPlayerAcceptReason::GiverMissing;

    if (giver.RuntimeGuid != instance.GiverRuntimeGuid)
        return DynamicQuestPlayerAcceptReason::GiverChanged;

    if (!giver.Materialized || !giver.AIWorldControlled || !giver.Alive)
        return DynamicQuestPlayerAcceptReason::GiverUnavailable;

    if (player.MapId != giver.MapId)
        return DynamicQuestPlayerAcceptReason::OutOfRange;

    // Milestone 2.13C3 P2 fix (STATIC review): fails closed on a
    // misconfigured server policy value BEFORE it is ever compared
    // against the live distance - a NaN maxInteractionRangeYards would
    // otherwise make every `distance > maxInteractionRangeYards`
    // comparison below false (NaN comparisons are always false), and a
    // +Infinity value would never bound anything, either way silently
    // admitting any distance on the same map.
    if (!std::isfinite(maxInteractionRangeYards) || maxInteractionRangeYards < 1.0f)
        return DynamicQuestPlayerAcceptReason::InteractionRangeInvalid;

    if (!std::isfinite(playerToGiverDistanceYards) ||
        playerToGiverDistanceYards < 0.0f ||
        playerToGiverDistanceYards > maxInteractionRangeYards)
        return DynamicQuestPlayerAcceptReason::OutOfRange;

    return DynamicQuestPlayerAcceptReason::None;
}
