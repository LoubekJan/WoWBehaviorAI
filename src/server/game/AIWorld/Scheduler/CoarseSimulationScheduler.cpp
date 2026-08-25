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
    // 0 ("never set" - reachable only defensively, see
    // SimulationScheduleState.h) is always immediately due and always
    // sorts first.
    uint64 NextTickAtMs(std::unordered_map<uint64, SimulationScheduleState> const& state, AgentId agent)
    {
        auto it = state.find(agent.Value);
        return it != state.end() ? it->second.NextTickAtMs : 0;
    }
}

CoarseSimulationScheduler::SelectionResult CoarseSimulationScheduler::SelectDue(
    std::unordered_map<uint64, SimulationScheduleState> const& state,
    std::vector<AgentId> const& candidates, uint64 nowMs, uint32 maxAdmit) const
{
    std::vector<AgentId> due;
    due.reserve(candidates.size());

    for (AgentId agent : candidates)
        if (NextTickAtMs(state, agent) <= nowMs)
            due.push_back(agent);

    std::sort(due.begin(), due.end(), [&](AgentId a, AgentId b)
    {
        uint64 aDueAtMs = NextTickAtMs(state, a);
        uint64 bDueAtMs = NextTickAtMs(state, b);
        if (aDueAtMs != bDueAtMs)
            return aDueAtMs < bDueAtMs;

        return a.Value < b.Value;
    });

    SelectionResult result;
    std::size_t admitCount = std::min<std::size_t>(due.size(), maxAdmit);

    result.Admitted.assign(due.begin(), due.begin() + admitCount);
    result.SkippedCapacity.assign(due.begin() + admitCount, due.end());

    return result;
}
