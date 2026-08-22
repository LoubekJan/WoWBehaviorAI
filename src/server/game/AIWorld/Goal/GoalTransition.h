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
#include <optional>

// What GoalSystem::UpdateActiveGoal() did this call - None on every tick
// the active goal (or lack of one) is simply retained, so a caller only
// needs to log the tick something actually changed.
enum class GoalTransition : uint8
{
    None,
    Activated,
    Interrupted,
    Released
};

inline char const* ToString(GoalTransition transition)
{
    switch (transition)
    {
        case GoalTransition::None:        return "NONE";
        case GoalTransition::Activated:   return "ACTIVATED";
        case GoalTransition::Interrupted: return "INTERRUPTED";
        case GoalTransition::Released:    return "RELEASED";
        default:                          return "UNKNOWN";
    }
}

// Goal is the new ActiveGoal to store (empty after a Release, or when
// nothing was ever activated). Pure value - the caller decides what to do
// with it, GoalSystem itself never touches AgentRecord.
struct GoalSelectionResult
{
    std::optional<ActiveGoal> Goal;
    GoalTransition Transition = GoalTransition::None;
};

#endif // AIWORLD_GOALTRANSITION_H
