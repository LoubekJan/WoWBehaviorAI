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

#ifndef AIWORLD_ACTIONRESULT_H
#define AIWORLD_ACTIONRESULT_H

#include "ActionType.h"
#include "Agent/AgentId.h"
#include "Define.h"
#include "Goal/GoalType.h"

// Milestone 2.8C: what ActionExecutor actually did with an already-ALLOWED
// ActionRequest - distinct from ActionValidationResult (may this run at
// all?) and GoalCompletion (why did the goal that requested it end?).
// Started, not "Executed": for Flee this only means MoveFleeing() was
// issued, not that the flee itself later succeeds - whether it does is
// still GoalCompletion's call once SafetyPressure drops or the goal times
// out, not this class's.
enum class ActionExecutionStatus : uint8
{
    Started,
    Failed
};

inline char const* ToString(ActionExecutionStatus status)
{
    switch (status)
    {
        case ActionExecutionStatus::Started: return "STARTED";
        case ActionExecutionStatus::Failed:  return "FAILED";
        default:                             return "UNKNOWN";
    }
}

enum class ActionExecutionReason : uint8
{
    None,
    UnsupportedAction,
    EngineRejected
};

inline char const* ToString(ActionExecutionReason reason)
{
    switch (reason)
    {
        case ActionExecutionReason::None:              return "NONE";
        case ActionExecutionReason::UnsupportedAction: return "UNSUPPORTED_ACTION";
        case ActionExecutionReason::EngineRejected:    return "ENGINE_REJECTED";
        default:                                       return "UNKNOWN";
    }
}

// Pure value: no Creature*/Unit*/Map* - ActionExecutor builds this from
// the ActionRequest it was given plus whatever happened during the call,
// and hands it back to the (world-thread) caller, the same handoff
// pattern GoalSelectionResult already uses for goals.
struct ActionResult
{
    AgentId Actor;
    ActionType Type = ActionType::Flee;

    ActionExecutionStatus Status = ActionExecutionStatus::Failed;
    ActionExecutionReason Reason = ActionExecutionReason::None;

    GoalType SourceGoal = GoalType::FleeDanger;
    uint64 GoalStartedAtMs = 0;
};

#endif // AIWORLD_ACTIONRESULT_H
