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

#ifndef AIWORLD_ROUTINESYSTEM_H
#define AIWORLD_ROUTINESYSTEM_H

#include "ActiveGoal.h"
#include "Agent/AgentLocation.h"
#include "Define.h"
#include "RoutineGoal.h"
#include "RoutineScheduleConfig.h"
#include <optional>

// Milestone 2.11B: deliberately NOT part of GoalSystem/GoalCandidate/
// ActiveGoal's Needs-driven state machine - a routine goal has no Need
// behind it (nothing to compute Utility from, no retention/timeout to
// check), so folding it into GoalSystem::UpdateActiveGoal() would mean
// teaching that already-tested state machine a second, incompatible kind
// of goal. Instead this is a second, independent, pure decision: given
// the agent's current ActiveGoal (to check for an Emergency override) and
// its HomeLocation/WorkLocation, what does its routine say right now.
// AIWorldMgr calls both this and GoalSystem every Needs-cadence tick and
// stores each result separately (AgentRecord::ActiveGoalState vs
// ::RoutineGoalState) - a later milestone is what reconciles a
// RoutineGoalState into an actual MOVE_TO ActionRequest; 2.11B itself
// produces the value and nothing else, no Action API, no world mutation.
// Pure value transform: no AgentId, Creature*, Map*, or DB.
class TC_GAME_API RoutineSystem
{
    public:
        // std::nullopt when: home or work is unset (not a routine-eligible
        // agent - eligibility is purely "has both HomeLocation and
        // WorkLocation", never AgentType, see AgentRecord.h), or
        // currentGoal is currently Emergency priority (e.g. an active
        // FLEE_DANGER) - the emergency override the roadmap's DoD asks
        // for. Otherwise deterministically GoToWork or GoHome, decided by
        // whether nowMs % config.DayLengthMs falls between config.WorkStartMs
        // (inclusive) and config.WorkEndMs (exclusive) - never by any
        // resolver-owned state, so calling this twice with the same
        // arguments always produces the same answer.
        std::optional<RoutineGoal> DeriveGoal(std::optional<ActiveGoal> const& currentGoal,
            std::optional<AgentLocation> const& home, std::optional<AgentLocation> const& work,
            uint64 nowMs, RoutineScheduleConfig const& config) const;
};

#endif // AIWORLD_ROUTINESYSTEM_H
