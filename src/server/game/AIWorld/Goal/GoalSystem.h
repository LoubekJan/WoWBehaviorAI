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

#ifndef AIWORLD_GOALSYSTEM_H
#define AIWORLD_GOALSYSTEM_H

#include "Define.h"
#include "GoalCandidate.h"
#include "Needs/NeedsState.h"
#include <vector>

// Milestone 2.7A: deterministic, level-triggered goal candidate generation
// from an agent's current NeedsState - never from NeedsThresholdEvent.
// A threshold event only fires on the crossing itself (see
// NeedsSystem::EvaluateThresholds()); Hunger sitting at 0.95 across many
// consecutive ticks produces no further HUNGER_CRITICAL event after the
// first, but the need for food obviously still exists every one of those
// ticks. GenerateCandidates() re-evaluates NeedsState directly each call,
// so a candidate exists for exactly as long as its Need stays at/above the
// threshold - no candidate-side latch/hysteresis in 2.7A (that's 2.7B's
// problem, once an ActiveGoal exists that must not flap every tick).
// Pure value transform: no AgentId, Creature*, Map*, AgentRecord*, or DB.
class TC_GAME_API GoalSystem
{
    public:
        std::vector<GoalCandidate> GenerateCandidates(NeedsState const& needs) const;
};

#endif // AIWORLD_GOALSYSTEM_H
