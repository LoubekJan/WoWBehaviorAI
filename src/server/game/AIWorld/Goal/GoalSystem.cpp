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
}

std::vector<GoalCandidate> GoalSystem::GenerateCandidates(NeedsState const& needs) const
{
    std::vector<GoalCandidate> candidates;

    if (needs.Hunger >= GoalCandidateThreshold)
        candidates.push_back({ GoalType::GetFood, GoalPriority::Normal, GoalSource::Needs, needs.Hunger });

    if (needs.SafetyPressure >= GoalCandidateThreshold)
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

GoalSelectionResult GoalSystem::UpdateActiveGoal(std::optional<ActiveGoal> const& current, NeedsState const& needs,
    std::vector<GoalCandidate> const& candidates, uint64 nowMs) const
{
    if (!current)
    {
        GoalCandidate const* best = SelectBest(candidates);
        if (!best)
            return {};

        ActiveGoal goal;
        goal.Type = best->Type;
        goal.Priority = best->Priority;
        goal.Source = best->Source;
        goal.Utility = best->Utility;
        goal.StartedAtMs = nowMs;

        return { goal, GoalTransition::Activated };
    }

    // A strictly higher-priority candidate always takes over immediately,
    // regardless of whether current's own Need has dropped - this is what
    // lets an EMERGENCY FLEE_DANGER interrupt an active NORMAL GET_FOOD.
    // The reverse never happens: a Normal candidate, however high its
    // Utility, cannot interrupt an active Emergency goal - only that
    // goal's own Need dropping below GoalRetentionThreshold releases it.
    GoalCandidate const* best = SelectBest(candidates);
    if (best && PriorityRank(best->Priority) > PriorityRank(current->Priority))
    {
        ActiveGoal goal;
        goal.Type = best->Type;
        goal.Priority = best->Priority;
        goal.Source = best->Source;
        goal.Utility = best->Utility;
        goal.StartedAtMs = nowMs;

        return { goal, GoalTransition::Interrupted };
    }

    if (NeedValueFor(current->Type, needs) < GoalRetentionThreshold)
        return { std::nullopt, GoalTransition::Released };

    return { current, GoalTransition::None };
}
