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

#include "GoalSystem.h"

namespace
{
    // Same 0.80 level NeedsSystem's threshold latch uses to enter ACTIVE -
    // deliberately not shared code with it, though: that constant gates an
    // edge-triggered audit event, this one gates a level-triggered
    // candidate, and 2.7B1 needs its own separate retention threshold
    // below anyway.
    constexpr float GoalCandidateThreshold = 0.80f;

    // 2.7B1 ActiveGoal retention: deliberately looser than
    // GoalCandidateThreshold. A candidate needs >= 0.80 to appear, but an
    // already-active goal is only released once its Need drops below 0.60
    // - the gap exists so an active goal doesn't flap every tick its Need
    // dips a hair under 0.80.
    constexpr float GoalRetentionThreshold = 0.60f;

    uint8 PriorityRank(GoalPriority priority)
    {
        switch (priority)
        {
            case GoalPriority::Emergency: return 2;
            case GoalPriority::Normal:    return 1;
            default:                      return 0;
        }
    }

    // The single Need a goal of this type is sustained by - what
    // UpdateActiveGoal() checks against GoalRetentionThreshold to decide
    // whether an active goal is still justified.
    float NeedValueFor(GoalType type, NeedsState const& needs)
    {
        switch (type)
        {
            case GoalType::GetFood:    return needs.Hunger;
            case GoalType::FleeDanger: return needs.SafetyPressure;
            default:                   return 0.0f;
        }
    }

    // Deterministic total order, independent of candidate vector order:
    // higher PriorityRank wins, then higher Utility, then a fixed ascending
    // GoalType ordinal as the final tie-break (arbitrary but stable/
    // reproducible - never left to whichever candidate happened to be
    // pushed first).
    bool IsBetterCandidate(GoalCandidate const& a, GoalCandidate const& b)
    {
        uint8 rankA = PriorityRank(a.Priority);
        uint8 rankB = PriorityRank(b.Priority);
        if (rankA != rankB)
            return rankA > rankB;

        if (a.Utility != b.Utility)
            return a.Utility > b.Utility;

        return uint8(a.Type) < uint8(b.Type);
    }

    // Milestone 2.7B2: a fixed per-GoalType default - no config knob yet.
    // 30s exists mainly so the FAILED path is actually runtime-observable
    // within this milestone's gate; it is not a tuned gameplay value.
    uint32 TimeoutFor(GoalType type)
    {
        switch (type)
        {
            case GoalType::GetFood:    return 30000;
            case GoalType::FleeDanger: return 30000;
            default:                   return 30000;
        }
    }
}

std::vector<GoalCandidate> GoalSystem::GenerateCandidates(NeedsState const& needs, bool hasFleeSource) const
{
    std::vector<GoalCandidate> candidates;

    if (needs.Hunger >= GoalCandidateThreshold)
        candidates.push_back({ GoalType::GetFood, GoalPriority::Normal, GoalSource::Needs, needs.Hunger });

    // Milestone 2.12G3D fix (STATIC review): hasFleeSource gates this
    // candidate's very existence - see this method's own header comment.
    if (needs.SafetyPressure >= GoalCandidateThreshold && hasFleeSource)
        candidates.push_back({ GoalType::FleeDanger, GoalPriority::Emergency, GoalSource::Needs, needs.SafetyPressure });

    return candidates;
}

GoalCandidate const* GoalSystem::SelectBest(std::vector<GoalCandidate> const& candidates) const
{
    GoalCandidate const* best = nullptr;

    for (GoalCandidate const& candidate : candidates)
    {
        if (!best || IsBetterCandidate(candidate, *best))
            best = &candidate;
    }

    return best;
}

ActiveGoal GoalSystem::MakeActiveGoal(GoalCandidate const& candidate, uint64 nowMs) const
{
    ActiveGoal goal;
    goal.Type = candidate.Type;
    goal.Priority = candidate.Priority;
    goal.Source = candidate.Source;
    goal.Utility = candidate.Utility;
    goal.StartedAtMs = nowMs;
    goal.TimeoutMs = TimeoutFor(candidate.Type);
    return goal;
}

GoalSelectionResult GoalSystem::UpdateActiveGoal(std::optional<ActiveGoal> const& current, NeedsState const& needs,
    std::vector<GoalCandidate> const& candidates, uint64 nowMs) const
{
    if (!current)
    {
        GoalCandidate const* best = SelectBest(candidates);
        if (!best)
            return {};

        return { MakeActiveGoal(*best, nowMs), GoalTransition::Activated };
    }

    // A strictly higher-priority candidate always takes over immediately,
    // regardless of whether current's own Need has dropped or its timeout
    // has elapsed - checked before success/timeout below specifically so
    // an emerging Emergency candidate never waits a tick just because
    // current also happens to satisfy its own success/timeout condition
    // this same tick. This is what lets an EMERGENCY FLEE_DANGER interrupt
    // an active NORMAL GET_FOOD. The reverse never happens: a Normal
    // candidate, however high its Utility, cannot interrupt an active
    // Emergency goal - only that goal's own Need dropping below
    // GoalRetentionThreshold (Succeeded) or its timeout elapsing (Failed)
    // ends it.
    GoalCandidate const* best = SelectBest(candidates);
    if (best && PriorityRank(best->Priority) > PriorityRank(current->Priority))
        return { MakeActiveGoal(*best, nowMs), GoalTransition::Interrupted };

    // Success condition is exactly the 2.7B1 retention threshold, just
    // renamed: the Need that justified this goal has dropped enough that
    // the goal is considered satisfied. Checked before timeout so a Need
    // that crosses below threshold on the very tick the timeout would also
    // fire counts as a success, not a failure.
    if (NeedValueFor(current->Type, needs) < GoalRetentionThreshold)
    {
        GoalCompletion completion;
        completion.Type = current->Type;
        completion.Status = GoalStatus::Succeeded;
        completion.Reason = GoalCompletionReason::NeedSatisfied;
        completion.StartedAtMs = current->StartedAtMs;
        completion.CompletedAtMs = nowMs;

        return { std::nullopt, GoalTransition::Succeeded, completion };
    }

    // nowMs >= StartedAtMs guards against StartedAtMs + TimeoutMs
    // overflowing past nowMs and comparing as "not yet timed out" - GameTime
    // is monotonic here in practice, but the subtraction form below is safe
    // regardless.
    bool timedOut = nowMs >= current->StartedAtMs && (nowMs - current->StartedAtMs) >= current->TimeoutMs;
    if (timedOut)
    {
        GoalCompletion completion;
        completion.Type = current->Type;
        completion.Status = GoalStatus::Failed;
        completion.Reason = GoalCompletionReason::Timeout;
        completion.StartedAtMs = current->StartedAtMs;
        completion.CompletedAtMs = nowMs;

        return { std::nullopt, GoalTransition::Failed, completion };
    }

    return { current, GoalTransition::None };
}
