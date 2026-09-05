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

#include "DynamicQuestCreation.h"
#include "Inference/QuestProposal.h"

#include <cmath>

char const* ToString(DynamicQuestCreateReason reason)
{
    switch (reason)
    {
        case DynamicQuestCreateReason::NotAttempted:      return "NOT_ATTEMPTED";
        case DynamicQuestCreateReason::None:              return "NONE";
        case DynamicQuestCreateReason::RegistryFull:      return "REGISTRY_FULL";
        case DynamicQuestCreateReason::GiverMissing:      return "GIVER_MISSING";
        case DynamicQuestCreateReason::GiverChanged:      return "GIVER_CHANGED";
        case DynamicQuestCreateReason::GiverUnavailable:  return "GIVER_UNAVAILABLE";
        case DynamicQuestCreateReason::TargetMissing:     return "TARGET_MISSING";
        case DynamicQuestCreateReason::TargetChanged:     return "TARGET_CHANGED";
        case DynamicQuestCreateReason::TargetUnavailable: return "TARGET_UNAVAILABLE";
        case DynamicQuestCreateReason::TargetOutOfRange:  return "TARGET_OUT_OF_RANGE";
        case DynamicQuestCreateReason::IdExhausted:       return "ID_EXHAUSTED";
        case DynamicQuestCreateReason::OfferRejected:     return "OFFER_REJECTED";
    }
    return "UNKNOWN";
}

DynamicQuestCreateReason CheckDynamicQuestCreateApplicability(
    QuestProposal const& proposal,
    DynamicQuestGiverFacts const& giver,
    DynamicQuestTargetFacts const& target)
{
    if (!giver.RecordExists)
        return DynamicQuestCreateReason::GiverMissing;

    if (giver.RuntimeGuid != proposal.GiverRuntimeGuid)
        return DynamicQuestCreateReason::GiverChanged;

    if (!giver.Materialized || !giver.AIWorldControlled || !giver.Alive)
        return DynamicQuestCreateReason::GiverUnavailable;

    if (!target.Resolved)
        return DynamicQuestCreateReason::TargetMissing;

    if (target.Entry != proposal.TargetEntry || target.MapId != proposal.TargetMapId)
        return DynamicQuestCreateReason::TargetChanged;

    if (!target.Alive || !target.Attackable)
        return DynamicQuestCreateReason::TargetUnavailable;

    if (!std::isfinite(target.GiverToTargetDistanceYards) ||
        target.GiverToTargetDistanceYards < 0.0f ||
        target.GiverToTargetDistanceYards > proposal.MaxRangeYards)
        return DynamicQuestCreateReason::TargetOutOfRange;

    return DynamicQuestCreateReason::None;
}
