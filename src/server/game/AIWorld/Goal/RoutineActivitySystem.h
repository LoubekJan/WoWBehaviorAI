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

#ifndef AIWORLD_ROUTINEACTIVITYSYSTEM_H
#define AIWORLD_ROUTINEACTIVITYSYSTEM_H

#include "Define.h"
#include "RoutineActivityContext.h"
#include "RoutineActivityType.h"
#include <optional>

// Milestone 2.11D: the layer between "arrived" and "acting" -
// RoutineGoal (GoToWork/GoHome) -> MOVE_TO -> ARRIVED -> RoutineActivity
// (Work/Rest), not a fourth movement/goal system of its own. Deliberately
// a separate class from RoutineSystem: DeriveGoal() decides WHERE from
// time+config+HomeLocation/WorkLocation, entirely independent of the
// actor's live position; DeriveActivity() decides WHAT ONCE THERE purely
// from reality-checked facts (materialized, alive, actually standing at
// the target, nothing else owning the actor) - two different kinds of
// input, kept in two different pure functions rather than one doing both.
//
// This first commit only produces the value and AIWorldMgr only logs the
// transition - no ActionRequest, no emote, no ResourcePressure/money
// change, no Action API at all. A later milestone reconciles
// RoutineActivityState into an actual observable action the same way
// 2.11C reconciled RoutineGoalState into MOVE_TO.
class TC_GAME_API RoutineActivitySystem
{
    public:
        // std::nullopt whenever WORK/REST is not currently justified:
        // unmaterialized, dead, ActiveGoalState/ActiveActionState owns the
        // actor, no RoutineGoalState at all, or not actually standing at
        // its target yet (still traveling). Otherwise GoalType::GoToWork ->
        // Work, GoalType::GoHome -> Rest - deterministic, no state of its
        // own, so calling this twice with the same context always produces
        // the same answer.
        std::optional<RoutineActivityType> DeriveActivity(RoutineActivityContext const& context) const;
};

#endif // AIWORLD_ROUTINEACTIVITYSYSTEM_H
