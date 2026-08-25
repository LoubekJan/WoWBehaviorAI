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

#include "CoarseSimulationScheduler.h"
#include <algorithm>

namespace
{
    // 0 ("never ticked, or just entered this tier") is always immediately
    // due and always sorts first - see this class's own header comment.
    uint64 EffectiveDueAtMs(std::unordered_map<uint64, SimulationScheduleState> const& state,
        CoarseSimulationScheduler::Candidate const& candidate, uint32 backgroundIntervalMs, uint32 abstractIntervalMs)
    {
        auto it = state.find(candidate.Agent.Value);
        uint64 lastTickAtMs = it != state.end() ? it->second.LastTickAtMs : 0;
        if (lastTickAtMs == 0)
            return 0;

        uint32 intervalMs = candidate.Tier == SimulationTier::Abstract ? abstractIntervalMs : backgroundIntervalMs;
        return lastTickAtMs + intervalMs;
    }
}

CoarseSimulationScheduler::SelectionResult CoarseSimulationScheduler::SelectDue(
    std::unordered_map<uint64, SimulationScheduleState> const& state,
    std::vector<Candidate> const& candidates, uint64 nowMs, uint32 maxAdmit,
    uint32 backgroundIntervalMs, uint32 abstractIntervalMs) const
{
    std::vector<Candidate> due;
    due.reserve(candidates.size());

    for (Candidate const& candidate : candidates)
        if (EffectiveDueAtMs(state, candidate, backgroundIntervalMs, abstractIntervalMs) <= nowMs)
            due.push_back(candidate);

    std::sort(due.begin(), due.end(), [&](Candidate const& a, Candidate const& b)
    {
        uint64 aDueAtMs = EffectiveDueAtMs(state, a, backgroundIntervalMs, abstractIntervalMs);
        uint64 bDueAtMs = EffectiveDueAtMs(state, b, backgroundIntervalMs, abstractIntervalMs);
        if (aDueAtMs != bDueAtMs)
            return aDueAtMs < bDueAtMs;

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
