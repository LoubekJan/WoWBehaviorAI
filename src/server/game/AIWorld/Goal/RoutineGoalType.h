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

#ifndef AIWORLD_ROUTINEGOALTYPE_H
#define AIWORLD_ROUTINEGOALTYPE_H

#include "Define.h"

// Milestone 2.11B: "where the agent's routine says it should be", not what
// it should do once there - actually working/resting at the destination is
// a later milestone's GoalType (see the roadmap's WORK/REST catalog).
// Deliberately not a GoalType/GoalCandidate: routine has no Need behind it
// to drive utility/retention/timeout, and is never in competition with
// GET_FOOD/FLEE_DANGER for GoalSystem::SelectBest() - see RoutineSystem.h.
enum class RoutineGoalType : uint8
{
    GoToWork,
    GoHome
};

inline char const* ToString(RoutineGoalType type)
{
    switch (type)
    {
        case RoutineGoalType::GoToWork: return "GO_TO_WORK";
        case RoutineGoalType::GoHome:   return "GO_HOME";
        default:                        return "UNKNOWN";
    }
}

#endif // AIWORLD_ROUTINEGOALTYPE_H
