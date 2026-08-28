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

#ifndef AIWORLD_GROUPCOORDINATIONGOAL_H
#define AIWORLD_GROUPCOORDINATIONGOAL_H

#include "Agent/GroupId.h"
#include "Define.h"
#include "GoalType.h"

// Milestone 2.12F2: an agent's own goal-like state sourced from GROUP
// coordination (AIWorldMgr::DispatchGroupMemberActionProposal(), itself
// fed by AgentGroupIntentSystem/AgentGroupIntentProjector - see
// AgentGroupIntent.h for the group-level/individual-level boundary this
// crosses) - the third and LOWEST-priority tier in the same "who owns the
// action slot" arbitration RoutineGoal already established as the second
// tier under Needs-driven ActiveGoalState (see AIWorldMgr::UpdateNeeds()'s
// own 2.11C/2.12F2 arbitration comments: Emergency ActiveGoal > Normal
// ActiveGoal > RoutineGoal > Regroup). Type is always GoalType::Regroup
// today - reusing GoalType (rather than a separate enum), the same reason
// RoutineGoal.h's own comment already gives for GoToWork/GoHome, is what
// lets this double as ActionRequest::SourceGoal/ActiveAction::SourceGoal
// directly, going through the exact same ActionSystem::Validate()/
// ActionExecutor pipeline every other MOVE_TO source already does.
//
// Unlike RoutineGoal (stateless, recomputed fresh every Needs tick), this
// is ephemeral - set only for the duration of one dispatched MOVE_TO
// attempt (StartedAtMs is this attempt's own identity, the same role
// ActiveGoal::StartedAtMs already plays, checked the same way by
// AIWorldMgr::ProcessActionEngineEvent()'s ownership discrimination), and
// cleared once that attempt reaches any terminal outcome
// (AIWorldMgr::HandleActionCompletion(), or one of the explicit
// preemption/cleanup sites in UpdateNeeds() that bypass it). Group
// coordination is not re-evaluated every Needs tick the way Routine is -
// the next relevant fact always comes from the next scheduled
// AIWorldMgr::RunCoalitionCoordination() pass instead, so there is
// nothing for a stale GroupCoordinationGoalState to keep meaning once its
// own MOVE_TO attempt has ended; unlike RoutineGoalState, it must not
// survive past that point.
//
// SourceGroup is identity/observability only (which group's own intent
// produced this attempt) - never consulted by ActionSystem/ActionExecutor,
// and never re-validated against AgentGroupRegistry once the MOVE_TO is
// already in flight (that revalidation already happened once, in
// DispatchGroupMemberActionProposal(), before this was ever set - a
// dissolve/leave that happens while this attempt is already moving simply
// lets the movement run to its own natural conclusion, the same way an
// in-flight MOVE_TO is never retroactively invalidated by anything else
// in this codebase either).
struct GroupCoordinationGoal
{
    GoalType Type = GoalType::Regroup;
    GroupId SourceGroup;
    uint64 StartedAtMs = 0;
};

#endif // AIWORLD_GROUPCOORDINATIONGOAL_H
