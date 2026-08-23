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

#ifndef AIWORLD_PENDINGEATCONTINUATION_H
#define AIWORLD_PENDINGEATCONTINUATION_H

#include "ActionPosition.h"
#include "Define.h"
#include "Goal/GoalType.h"

// Milestone 2.8G P2 fix: records that a MOVE_TO just arrived at a
// GET_FOOD target, without acting on it yet - HandleActionCompletion()
// sets this instead of immediately proposing/validating/executing Eat, so
// that this same tick's GenerateCandidates()/UpdateActiveGoal() pass gets
// to run first. Without the delay, a same-tick memory-driven
// SafetyPressure spike could let Eat go through a moment before an
// emergency FLEE_DANGER would otherwise have interrupted GET_FOOD -
// GoalSystem's Emergency-beats-Normal guarantee must see the goal
// snapshot before Eat commits to it, not after. One-shot: consumed
// (validated against the post-selection ActiveGoalState, then cleared)
// exactly once, whether or not it actually produces an Eat.
struct PendingEatContinuation
{
    ActionPosition Destination;
    GoalType SourceGoal = GoalType::GetFood;
    uint64 GoalStartedAtMs = 0;
};

#endif // AIWORLD_PENDINGEATCONTINUATION_H
