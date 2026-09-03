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

#include "DynamicTaskValidation.h"
#include "DynamicTaskCandidate.h"
#include "QuestProposalDraft.h"
#include "QuestRequestProvenance.h"

#include <cmath>

char const* ToString(DynamicTaskValidationReason reason)
{
    switch (reason)
    {
        case DynamicTaskValidationReason::None:                  return "NONE";
        case DynamicTaskValidationReason::UnsupportedObjective:   return "UNSUPPORTED_OBJECTIVE";
        case DynamicTaskValidationReason::TargetBindingMissing:   return "TARGET_BINDING_MISSING";
        case DynamicTaskValidationReason::TargetBindingMismatch:  return "TARGET_BINDING_MISMATCH";
        case DynamicTaskValidationReason::RequiredCountInvalid:   return "REQUIRED_COUNT_INVALID";
        case DynamicTaskValidationReason::RangeInvalid:           return "RANGE_INVALID";
        case DynamicTaskValidationReason::ExpiryInvalid:          return "EXPIRY_INVALID";
        case DynamicTaskValidationReason::RewardInvalid:          return "REWARD_INVALID";
        case DynamicTaskValidationReason::LiveTargetOutOfRange:   return "LIVE_TARGET_OUT_OF_RANGE";
    }
    return "UNKNOWN";
}

namespace
{
    QuestTargetBinding const* FindBinding(DynamicTaskCandidate const& candidate)
    {
        for (QuestTargetBinding const& binding : candidate.Provenance.TargetBindings)
            if (binding.Token == candidate.Draft.TargetToken)
                return &binding;
        return nullptr;
    }
}

DynamicTaskValidationReason ValidateDynamicTaskCandidate(
    DynamicTaskCandidate const& candidate,
    DynamicTaskAuthoritativeLimits const& limits,
    DynamicTaskWorldFacts const& worldFacts)
{
    QuestProposalDraft const& draft = candidate.Draft;

    if (draft.Objective != QuestObjectiveType::KillCreature)
        return DynamicTaskValidationReason::UnsupportedObjective;

    QuestTargetBinding const* binding = FindBinding(candidate);
    if (!binding)
        return DynamicTaskValidationReason::TargetBindingMissing;

    if (binding->Entry != worldFacts.TargetEntry || binding->MapId != worldFacts.TargetMapId)
        return DynamicTaskValidationReason::TargetBindingMismatch;

    if (draft.RequiredCount == 0 || draft.RequiredCount > limits.MaxRequiredCount)
        return DynamicTaskValidationReason::RequiredCountInvalid;

    if (!std::isfinite(draft.MaxRangeYards) || draft.MaxRangeYards <= 0.0f || draft.MaxRangeYards > limits.MaxRangeYards)
        return DynamicTaskValidationReason::RangeInvalid;

    if (draft.ExpiryMs == 0 || draft.ExpiryMs > limits.MaxExpiryMs)
        return DynamicTaskValidationReason::ExpiryInvalid;

    if (draft.RewardMoneyCopper > limits.MaxRewardMoneyCopper)
        return DynamicTaskValidationReason::RewardInvalid;

    if (!std::isfinite(worldFacts.TargetDistanceYards) || worldFacts.TargetDistanceYards > draft.MaxRangeYards)
        return DynamicTaskValidationReason::LiveTargetOutOfRange;

    return DynamicTaskValidationReason::None;
}
