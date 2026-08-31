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
// ActiveGoal > RoutineGoal > Regroup/Roam). Type is GoalType::Regroup or,
// since 2.12G2, GoalType::Roam - whichever group-coordination-sourced
// MOVE_TO this specific attempt actually is (see
// DispatchGroupMemberActionProposal()'s own mapping from
// GroupMemberActionProposal::SourceIntent). Reusing GoalType (rather than
// a separate enum), the same reason RoutineGoal.h's own comment already
// gives for GoToWork/GoHome, is what lets this double as
// ActionRequest::SourceGoal/ActiveAction::SourceGoal directly, going
// through the exact same ActionSystem::Validate()/ActionExecutor pipeline
// every other MOVE_TO source already does.
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
// which never re-validate against AgentGroupRegistry once the MOVE_TO is
// already in flight (that revalidation already happened once, in
// DispatchGroupMemberActionProposal(), before this was ever set).
//
// Milestone 2.12F2 P2 fix (STATIC review): SourceGroup IS actively used
// outside that ActionSystem/ActionExecutor pipeline, though - a confirmed
// Leave/Dissolve for this exact GroupId (AIWorldMgr::RequestLeaveGroupWithPolicy()/
// RequestDissolveGroup()'s own completions) calls
// AIWorldMgr::StopGroupCoordinationForMember() for every affected member,
// which compares SourceGroup against the group that just changed and stops
// the movement if they match. An earlier version of this comment claimed a
// dissolve/leave mid-flight "simply lets the movement run to its own
// natural conclusion" - STATIC review correctly identified that as a real
// bug: the member would keep walking toward a group's own territory point
// after that group (or its own membership in it) no longer exists, a stale
// group-owned action with no group behind it any more. Every OTHER
// in-flight MOVE_TO in this codebase is still never retroactively
// invalidated by anything else - this is deliberately the one exception,
// scoped narrowly to "the group this attempt's own identity names just
// stopped applying to this member".
struct GroupCoordinationGoal
{
    GoalType Type = GoalType::Regroup;
    GroupId SourceGroup;
    uint64 StartedAtMs = 0;
};

#endif // AIWORLD_GROUPCOORDINATIONGOAL_H
