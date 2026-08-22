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

#include "ActiveGoal.h"
#include "Define.h"
#include "GoalCandidate.h"
#include "GoalTransition.h"
#include "Needs/NeedsState.h"
#include <optional>
#include <vector>

// Milestone 2.7A: deterministic, level-triggered goal candidate generation
// from an agent's current NeedsState - never from NeedsThresholdEvent.
// A threshold event only fires on the crossing itself (see
// NeedsSystem::EvaluateThresholds()); Hunger sitting at 0.95 across many
// consecutive ticks produces no further HUNGER_CRITICAL event after the
// first, but the need for food obviously still exists every one of those
// ticks. GenerateCandidates() re-evaluates NeedsState directly each call,
// so a candidate exists for exactly as long as its Need stays at/above the
// threshold - no candidate-side latch/hysteresis in 2.7A (that's 2.7B1's
// problem, once an ActiveGoal exists that must not flap every tick).
// Pure value transform: no AgentId, Creature*, Map*, AgentRecord*, or DB.
class TC_GAME_API GoalSystem
{
    public:
        std::vector<GoalCandidate> GenerateCandidates(NeedsState const& needs) const;

        // Milestone 2.7B1: current is the agent's existing ActiveGoal (or
        // empty if it has none); candidates is this tick's
        // GenerateCandidates() output. Deterministic, one transition per
        // call:
        //   - no current goal -> selects the best candidate (Emergency
        //     outranks Normal, then higher Utility, then a fixed GoalType
        //     tie-break) and Activates it, or does nothing if there are no
        //     candidates.
        //   - a higher-priority candidate exists than current ->
        //     Interrupts current with it, regardless of whether current's
        //     own Need has dropped.
        //   - otherwise, if current's own Need has dropped below the
        //     retention threshold -> Releases it.
        //   - otherwise -> keeps current unchanged (Transition::None).
        // Retention is deliberately looser than activation (candidates
        // need >= 0.80 to appear, but an already-active goal is only
        // released once its Need drops < 0.60) so a goal doesn't flap
        // every tick its Need dips a hair below the activation threshold.
        // Pure value in, pure value out - never touches AgentRecord
        // itself; the caller is responsible for storing the result.
        GoalSelectionResult UpdateActiveGoal(std::optional<ActiveGoal> const& current, NeedsState const& needs,
            std::vector<GoalCandidate> const& candidates, uint64 nowMs) const;

    private:
        GoalCandidate const* SelectBest(std::vector<GoalCandidate> const& candidates) const;
};

#endif // AIWORLD_GOALSYSTEM_H
