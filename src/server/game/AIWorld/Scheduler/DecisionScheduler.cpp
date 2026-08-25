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

#include "DecisionScheduler.h"
#include <algorithm>

namespace
{
    // Milestone 2.10B P2 fix: 0 ("never submitted/attempted") is always
    // immediately due - returning 0 here rather than folding it into the
    // +interval arithmetic below both keeps that case obviously correct
    // (no interval added to a sentinel) and gives it the smallest possible
    // effectiveDueAtMs, so a never-attempted agent also always sorts
    // first among due candidates.
    uint64 EffectiveDueAtMs(std::unordered_map<uint64, DecisionScheduleState> const& state,
        DecisionScheduler::Candidate const& candidate, uint32 nearbyIntervalMs, uint32 activeIntervalMs)
    {
        auto it = state.find(candidate.Agent.Value);
        uint64 lastSubmittedAtMs = it != state.end() ? it->second.LastDecisionSubmittedAtMs : 0;
        if (lastSubmittedAtMs == 0)
            return 0;

        uint32 intervalMs = candidate.CadenceClass == DecisionCadenceClass::Nearby ? nearbyIntervalMs : activeIntervalMs;
        return lastSubmittedAtMs + intervalMs;
    }
}

DecisionScheduler::SelectionResult DecisionScheduler::SelectDue(
    std::unordered_map<uint64, DecisionScheduleState> const& state,
    std::vector<Candidate> const& candidates, uint64 nowMs, uint32 maxAdmit,
    uint32 nearbyIntervalMs, uint32 activeIntervalMs) const
{
    std::vector<Candidate> due;
    due.reserve(candidates.size());

    for (Candidate const& candidate : candidates)
    {
        auto it = state.find(candidate.Agent.Value);
        if (it != state.end() && it->second.AwaitingResponse)
            continue;

        if (EffectiveDueAtMs(state, candidate, nearbyIntervalMs, activeIntervalMs) > nowMs)
            continue;

        due.push_back(candidate);
    }

    std::sort(due.begin(), due.end(), [&](Candidate const& a, Candidate const& b)
    {
        uint64 aDueAtMs = EffectiveDueAtMs(state, a, nearbyIntervalMs, activeIntervalMs);
        uint64 bDueAtMs = EffectiveDueAtMs(state, b, nearbyIntervalMs, activeIntervalMs);

        // Primary key: whichever candidate has been due longer wins,
        // regardless of class - this is what bounds Active starvation,
        // see this class's own header comment for why.
        if (aDueAtMs != bDueAtMs)
            return aDueAtMs < bDueAtMs;

        // Only a tie-break between two candidates equally overdue.
        if (a.CadenceClass != b.CadenceClass)
            return a.CadenceClass == DecisionCadenceClass::Nearby;

        return a.Agent.Value < b.Agent.Value;
    });

    SelectionResult result;
    std::size_t admitCount = std::min<std::size_t>(due.size(), maxAdmit);

    result.Admitted.reserve(admitCount);
    for (std::size_t i = 0; i < admitCount; ++i)
        result.Admitted.push_back(due[i].Agent);

    result.SkippedCapacity.reserve(due.size() - admitCount);
    for (std::size_t i = admitCount; i < due.size(); ++i)
        result.SkippedCapacity.push_back(due[i].Agent);

    return result;
}
