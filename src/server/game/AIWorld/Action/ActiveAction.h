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

#ifndef AIWORLD_ACTIVEACTION_H
#define AIWORLD_ACTIVEACTION_H

#include "ActionPosition.h"
#include "ActionType.h"
#include "Define.h"
#include "Goal/GoalType.h"
#include <optional>

// Milestone 2.8F: the one Action currently actually running in TrinityCore
// for an agent - as of 2.8F, only ever a MoveTo, set after
// ActionExecutor::ExecuteMoveTo() has actually returned Started, never
// before validation and never on a Failed result. Flee is not tracked
// here yet - its lifecycle is still driven purely by GoalCompletion
// (StopFlee() is called directly from FLEE_DANGER's own transitions, with
// no ActiveActionState involved); folding Flee in too is a natural
// extension once something needs to reason about "is any action running"
// generically, not a 2.8F requirement. Owned by AgentRecord alongside
// NeedsThresholds/ActiveGoalState, for the same reason: it must survive
// Creature unload/reload, and every path that ends the underlying engine
// movement (interrupt, goal completion, dematerialization, natural
// arrival) must also clear this - an ActiveAction must never claim a
// movement that isn't actually running anymore. Pure value: no Creature*,
// Unit*, Map*, or DB.
struct ActiveAction
{
    ActionType Type = ActionType::MoveTo;

    // What ActiveGoal attempt this action belongs to - the same identity
    // (GoalType + StartedAtMs) ActionRequest already carries, kept here so
    // a later arrival/completion event can be checked against the actual
    // current ActiveGoal before being trusted.
    GoalType SourceGoal = GoalType::GetFood;
    uint64 GoalStartedAtMs = 0;

    uint64 StartedAtMs = 0;
    std::optional<ActionPosition> Destination;
};

#endif // AIWORLD_ACTIVEACTION_H
