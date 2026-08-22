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
    // candidate, and 2.7B may need to move these apart once a candidate
    // needs its own hysteresis.
    constexpr float GoalCandidateThreshold = 0.80f;
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
