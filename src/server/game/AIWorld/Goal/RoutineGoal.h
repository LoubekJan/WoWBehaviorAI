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

#ifndef AIWORLD_ROUTINEGOAL_H
#define AIWORLD_ROUTINEGOAL_H

#include "GoalTarget.h"
#include "RoutineGoalType.h"

// Milestone 2.11B: RoutineSystem's only output - which of HomeLocation/
// WorkLocation the agent's routine currently points at, already resolved
// to a GoalTarget (reusing the same DTO FoodTargetResolver produces, for
// the same reason: this is "where", not an ActionRequest). A later
// milestone translates this into a MOVE_TO via ActionSystem; 2.11B itself
// never touches Action API. Pure value: no AgentId, Creature*, Map*, or DB.
struct RoutineGoal
{
    RoutineGoalType Type = RoutineGoalType::GoHome;
    GoalTarget Target;
};

#endif // AIWORLD_ROUTINEGOAL_H
