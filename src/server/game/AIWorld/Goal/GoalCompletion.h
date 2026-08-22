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

#ifndef AIWORLD_GOALCOMPLETION_H
#define AIWORLD_GOALCOMPLETION_H

#include "Define.h"
#include "GoalStatus.h"
#include "GoalType.h"

// Milestone 2.7B2: what a just-finished ActiveGoal was and how it ended.
// Only produced alongside GoalTransition::Succeeded/Failed - death
// (GoalTransition::Released) is an external cancellation, not an outcome
// the goal itself reached, so it never gets a GoalCompletion. Pure value:
// no AgentId, Creature*, Map*, or DB.
struct GoalCompletion
{
    GoalType Type = GoalType::GetFood;
    GoalStatus Status = GoalStatus::Succeeded;
    GoalCompletionReason Reason = GoalCompletionReason::NeedSatisfied;

    uint64 StartedAtMs = 0;
    uint64 CompletedAtMs = 0;
};

#endif // AIWORLD_GOALCOMPLETION_H
