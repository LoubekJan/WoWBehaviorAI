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
#include "QuestContext.h"
#include "QuestContractLimits.h"
#include "QuestProposalDraft.h"
#include "QuestRequestProvenance.h"

#include <cmath>

char const* ToString(DynamicTaskValidationReason reason)
{
    switch (reason)
    {
        case DynamicTaskValidationReason::NotValidated:          return "NOT_VALIDATED";
        case DynamicTaskValidationReason::None:                  return "NONE";
        case DynamicTaskValidationReason::UnsupportedObjective:  return "UNSUPPORTED_OBJECTIVE";
        case DynamicTaskValidationReason::SourceProblemMismatch: return "SOURCE_PROBLEM_MISMATCH";
        case DynamicTaskValidationReason::TargetBindingMissing:   return "TARGET_BINDING_MISSING";
        case DynamicTaskValidationReason::TargetBindingAmbiguous: return "TARGET_BINDING_AMBIGUOUS";
        case DynamicTaskValidationReason::TargetBindingMismatch:  return "TARGET_BINDING_MISMATCH";
        case DynamicTaskValidationReason::RequiredCountInvalid:  return "REQUIRED_COUNT_INVALID";
        case DynamicTaskValidationReason::RangeInvalid:          return "RANGE_INVALID";
        case DynamicTaskValidationReason::ExpiryInvalid:         return "EXPIRY_INVALID";
        case DynamicTaskValidationReason::RewardInvalid:         return "REWARD_INVALID";
        case DynamicTaskValidationReason::TextInvalid:           return "TEXT_INVALID";
        case DynamicTaskValidationReason::LiveTargetOutOfRange:  return "LIVE_TARGET_OUT_OF_RANGE";
    }
    return "UNKNOWN";
}

namespace
{
    // Fails closed on ambiguity: candidate.Draft.TargetToken must resolve
    // to EXACTLY one binding. Zero matches and more than one match are
    // both treated as unresolved - a duplicate-token binding list is
    // never something to "pick the first of" - but reported as distinct
    // reasons via `ambiguous` so a caller can tell "the model named
    // nothing real" (TargetBindingMissing) apart from "the binding list
    // itself is malformed" (TargetBindingAmbiguous).
    QuestTargetBinding const* FindBinding(DynamicTaskCandidate const& candidate, bool& ambiguous)
    {
        ambiguous = false;
        QuestTargetBinding const* found = nullptr;
        for (QuestTargetBinding const& binding : candidate.Provenance.TargetBindings)
        {
            if (binding.Token != candidate.Draft.TargetToken)
                continue;
            if (found)
            {
                ambiguous = true;
                return nullptr;
            }
            found = &binding;
        }
        return found;
    }

    bool IsValidProposalText(std::string const& text, size_t maxLength)
    {
        if (text.empty() || text.size() > maxLength)
            return false;

        for (unsigned char ch : text)
            if (ch < 0x20 || ch == 0x7F) // ASCII control/DEL - no newline, tab, etc in player-facing text
                return false;

        return true;
    }
}

DynamicTaskValidationResult ValidateDynamicTaskCandidate(
    DynamicTaskCandidate const& candidate,
    DynamicTaskAuthoritativeLimits const& limits,
    DynamicTaskAuthoritativeFacts const& facts)
{
    DynamicTaskValidationResult result;

    QuestProposalDraft const& draft = candidate.Draft;

    if (draft.Objective != QuestObjectiveType::KillCreature)
    {
        result.Reason = DynamicTaskValidationReason::UnsupportedObjective;
        return result;
    }

    QuestProblemContext const& problem = candidate.RequestContext.Problem;
    if (facts.Source.Type != problem.Type ||
        facts.Source.ActorEntry != problem.ActorEntry ||
        facts.Source.TargetEntry != problem.TargetEntry ||
        facts.Source.MapId != problem.MapId)
    {
        result.Reason = DynamicTaskValidationReason::SourceProblemMismatch;
        return result;
    }

    bool bindingAmbiguous = false;
    QuestTargetBinding const* binding = FindBinding(candidate, bindingAmbiguous);
    if (!binding)
    {
        result.Reason = bindingAmbiguous
            ? DynamicTaskValidationReason::TargetBindingAmbiguous
            : DynamicTaskValidationReason::TargetBindingMissing;
        return result;
    }

    if (binding->Entry != facts.Target.Entry || binding->MapId != facts.Target.MapId)
    {
        result.Reason = DynamicTaskValidationReason::TargetBindingMismatch;
        return result;
    }

    if (draft.RequiredCount == 0 || draft.RequiredCount > limits.MaxRequiredCount)
    {
        result.Reason = DynamicTaskValidationReason::RequiredCountInvalid;
        return result;
    }

    if (!std::isfinite(limits.MaxRangeYards) || limits.MaxRangeYards <= 0.0f ||
        !std::isfinite(draft.MaxRangeYards) || draft.MaxRangeYards <= 0.0f ||
        draft.MaxRangeYards > limits.MaxRangeYards)
    {
        result.Reason = DynamicTaskValidationReason::RangeInvalid;
        return result;
    }

    if (draft.ExpiryMs == 0 || draft.ExpiryMs > limits.MaxExpiryMs)
    {
        result.Reason = DynamicTaskValidationReason::ExpiryInvalid;
        return result;
    }

    if (draft.RewardMoneyCopper > limits.MaxRewardMoneyCopper)
    {
        result.Reason = DynamicTaskValidationReason::RewardInvalid;
        return result;
    }

    if (!IsValidProposalText(draft.Title, QuestContractMaxTitleLength) ||
        !IsValidProposalText(draft.Description, QuestContractMaxDescriptionLength))
    {
        result.Reason = DynamicTaskValidationReason::TextInvalid;
        return result;
    }

    if (!std::isfinite(facts.Target.GiverToTargetDistanceYards) ||
        facts.Target.GiverToTargetDistanceYards < 0.0f ||
        facts.Target.GiverToTargetDistanceYards > draft.MaxRangeYards)
    {
        result.Reason = DynamicTaskValidationReason::LiveTargetOutOfRange;
        return result;
    }

    QuestProposal proposal;
    proposal.Giver = candidate.Provenance.Agent;
    proposal.GiverRuntimeGuid = candidate.Provenance.RuntimeGuid;
    proposal.SourceEventId = candidate.Provenance.SourceEventId;
    proposal.SourceEventType = candidate.Provenance.SourceEventType;
    proposal.Objective = draft.Objective;
    proposal.TargetToken = draft.TargetToken;
    proposal.TargetGuid = binding->Guid;
    proposal.TargetEntry = facts.Target.Entry;
    proposal.TargetMapId = facts.Target.MapId;
    proposal.RequiredCount = draft.RequiredCount;
    proposal.MaxRangeYards = draft.MaxRangeYards;
    proposal.ExpiryMs = draft.ExpiryMs;
    proposal.RewardMoneyCopper = draft.RewardMoneyCopper;
    proposal.Title = draft.Title;
    proposal.Description = draft.Description;

    result.Reason = DynamicTaskValidationReason::None;
    result.Proposal = std::move(proposal);
    return result;
}
