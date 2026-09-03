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

#include "DynamicTaskAcceptance.h"

namespace
{
    QuestTargetCandidate const* FindCandidate(std::vector<QuestTargetCandidate> const& candidates, uint32 token)
    {
        for (QuestTargetCandidate const& candidate : candidates)
            if (candidate.Token == token)
                return &candidate;
        return nullptr;
    }

    QuestTargetBinding const* FindBinding(std::vector<QuestTargetBinding> const& bindings, uint32 token)
    {
        for (QuestTargetBinding const& binding : bindings)
            if (binding.Token == token)
                return &binding;
        return nullptr;
    }
}

DynamicTaskDiscardReason CheckDynamicTaskResponseAcceptance(
    PendingDynamicTaskRequest const& pending,
    QuestProposalDraft const& draft,
    DynamicTaskAcceptanceState const& state)
{
    if (pending.Provenance.SnapshotSequence != state.CurrentSnapshotSequence)
        return DynamicTaskDiscardReason::StaleSnapshot;

    if (pending.Provenance.RuntimeGuid.IsEmpty() || pending.Provenance.RuntimeGuid != state.CurrentRuntimeGuid)
        return DynamicTaskDiscardReason::StaleRuntime;

    if (pending.Provenance.Goal)
    {
        if (!state.CurrentGoal ||
            *pending.Provenance.Goal != *state.CurrentGoal ||
            pending.Provenance.GoalStartedAtMs != state.CurrentGoalStartedAtMs)
            return DynamicTaskDiscardReason::StaleGoal;
    }

    if (!state.SourceEventStillActive)
        return DynamicTaskDiscardReason::StaleSourceEvent;

    if (state.NowMs < pending.SubmittedAtMs || state.NowMs - pending.SubmittedAtMs > state.ResponseMaxAgeMs)
        return DynamicTaskDiscardReason::StaleRequest;

    QuestTargetCandidate const* candidate = FindCandidate(pending.Context.CandidateTargets, draft.TargetToken);
    QuestTargetBinding const* binding = FindBinding(pending.Provenance.TargetBindings, draft.TargetToken);
    if (!candidate || !binding)
        return DynamicTaskDiscardReason::TargetTokenUnbound;

    if (candidate->Entry != binding->Entry || candidate->MapId != binding->MapId)
        return DynamicTaskDiscardReason::TargetBindingMismatch;

    QuestProposalLimits const& limits = pending.Context.Limits;
    if (draft.RequiredCount > limits.MaxRequiredCount ||
        draft.MaxRangeYards > limits.MaxRangeYards ||
        draft.ExpiryMs > limits.MaxExpiryMs ||
        draft.RewardMoneyCopper > limits.MaxRewardMoneyCopper)
        return DynamicTaskDiscardReason::PolicyWindowMismatch;

    return DynamicTaskDiscardReason::None;
}

QuestTargetBinding const* ResolveDynamicTaskTargetBinding(
    PendingDynamicTaskRequest const& pending,
    QuestProposalDraft const& draft)
{
    return FindBinding(pending.Provenance.TargetBindings, draft.TargetToken);
}
