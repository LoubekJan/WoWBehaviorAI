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

DecisionScheduler::SelectionResult DecisionScheduler::SelectDue(
    std::unordered_map<uint64, DecisionScheduleState> const& state,
    std::vector<Candidate> const& candidates, uint64 nowMs, uint32 maxAdmit) const
{
    std::vector<Candidate> due;
    due.reserve(candidates.size());

    for (Candidate const& candidate : candidates)
    {
        auto it = state.find(candidate.Agent.Value);

        // An agent the scheduler has never seen before has no entry yet -
        // DecisionScheduleState's own defaults (NextDecisionAtMs = 0,
        // AwaitingResponse = false) are exactly "due immediately, nothing
        // outstanding", so treating a missing entry as those defaults
        // needs no special-casing here.
        if (it != state.end() && it->second.AwaitingResponse)
            continue;

        uint64 nextDecisionAtMs = it != state.end() ? it->second.NextDecisionAtMs : 0;
        if (nextDecisionAtMs > nowMs)
            continue;

        due.push_back(candidate);
    }

    std::sort(due.begin(), due.end(), [&state](Candidate const& a, Candidate const& b)
    {
        // Nearby always outranks Active - see this header's own comment
        // for why that is an accepted, documented limitation for now.
        if (a.CadenceClass != b.CadenceClass)
            return a.CadenceClass == DecisionCadenceClass::Nearby;

        auto aIt = state.find(a.Agent.Value);
        auto bIt = state.find(b.Agent.Value);
        uint64 aNext = aIt != state.end() ? aIt->second.NextDecisionAtMs : 0;
        uint64 bNext = bIt != state.end() ? bIt->second.NextDecisionAtMs : 0;

        if (aNext != bNext)
            return aNext < bNext;

        return a.Agent.Value < b.Agent.Value;
    });

    SelectionResult result;
    std::size_t admitCount = std::min<std::size_t>(due.size(), maxAdmit);

    result.Admitted.assign(due.begin(), due.begin() + admitCount);

    result.SkippedCapacity.reserve(due.size() - admitCount);
    for (std::size_t i = admitCount; i < due.size(); ++i)
        result.SkippedCapacity.push_back(due[i].Agent);

    return result;
}
