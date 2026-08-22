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

#ifndef AIWORLD_GOALTRANSITION_H
#define AIWORLD_GOALTRANSITION_H

#include "ActiveGoal.h"
#include "Define.h"
#include "GoalCompletion.h"
#include <optional>

// What GoalSystem::UpdateActiveGoal() did this call - None on every tick
// the active goal (or lack of one) is simply retained, so a caller only
// needs to log the tick something actually changed. Succeeded/Failed are
// terminal outcomes UpdateActiveGoal() itself reaches (2.7B2); Released
// stays reserved for an external cancellation the goal didn't reach on its
// own - today that's only the death guard in AIWorldMgr::UpdateNeeds(),
// which releases outright without ever calling UpdateActiveGoal().
enum class GoalTransition : uint8
{
    None,
    Activated,
    Interrupted,
    Succeeded,
    Failed,
    Released
};

inline char const* ToString(GoalTransition transition)
{
    switch (transition)
    {
        case GoalTransition::None:        return "NONE";
        case GoalTransition::Activated:   return "ACTIVATED";
        case GoalTransition::Interrupted: return "INTERRUPTED";
        case GoalTransition::Succeeded:   return "SUCCEEDED";
        case GoalTransition::Failed:      return "FAILED";
        case GoalTransition::Released:    return "RELEASED";
        default:                          return "UNKNOWN";
    }
}

// Goal is the new ActiveGoal to store (empty after Succeeded/Failed/
// Release, or when nothing was ever activated). Completion is set only
// alongside Succeeded/Failed - a terminal outcome the goal itself reached,
// as opposed to Released's external cancellation. Pure value - the caller
// decides what to do with it, GoalSystem itself never touches AgentRecord.
struct GoalSelectionResult
{
    std::optional<ActiveGoal> Goal;
    GoalTransition Transition = GoalTransition::None;
    std::optional<GoalCompletion> Completion;
};

#endif // AIWORLD_GOALTRANSITION_H
