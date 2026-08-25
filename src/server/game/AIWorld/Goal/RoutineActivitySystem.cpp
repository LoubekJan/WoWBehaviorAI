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

#include "RoutineActivitySystem.h"

std::optional<RoutineActivityType> RoutineActivitySystem::DeriveActivity(RoutineActivityContext const& context) const
{
    if (!context.Materialized || !context.Alive)
        return std::nullopt;

    // Single-owner rule: ActiveGoalState/ActiveActionState always outrank
    // routine, the same as 2.11C's arbitration for movement - an agent
    // fleeing or fetching food is never simultaneously WORK/REST.
    if (context.HasActiveGoal || context.HasActiveAction)
        return std::nullopt;

    // 2.11D P3 fix: ArrivalToleranceYards alone only proves position, not
    // stillness - ActorMoving is the authoritative engine fact for that,
    // independent of whatever AIWorld's own ActiveActionState happens to
    // claim (see RoutineActivityContext.h).
    if (context.ActorMoving)
        return std::nullopt;

    if (!context.AtRoutineTarget || !context.CurrentRoutineGoal)
        return std::nullopt;

    switch (*context.CurrentRoutineGoal)
    {
        case GoalType::GoToWork: return RoutineActivityType::Work;
        case GoalType::GoHome:   return RoutineActivityType::Rest;
        default:                 return std::nullopt;
    }
}
