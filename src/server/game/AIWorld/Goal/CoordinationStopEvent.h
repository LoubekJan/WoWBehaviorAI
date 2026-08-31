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

#ifndef AIWORLD_COORDINATIONSTOPEVENT_H
#define AIWORLD_COORDINATIONSTOPEVENT_H

#include "Agent/GroupId.h"
#include "Define.h"
#include "GoalType.h"

// Milestone 2.12G2R P2 fix, round 2 (STATIC review): a group-coordination-
// owned MOVE_TO (GroupCoordinationGoal.h) can stop for two genuinely
// different production reasons - AIWorldMgr::UpdateNeeds()'s own
// COORDINATION_PREEMPTED_BY_GOAL block (a higher-priority individual goal
// took the action slot back) and AIWorldMgr::StopInFlightGroupCoordination()'s
// own COORDINATION_STOPPED_BY_LIFECYCLE path (a confirmed Leave/Dissolve
// stopped it) - and, separately, it can also simply ARRIVE at its own
// destination (AIWorldMgr::HandleActionCompletion()), which is not a
// "stop" in this sense at all. From OUTSIDE AgentRecord, all three end
// states look identical: ActiveActionState/GroupCoordinationGoalState both
// gone. That ambiguity is exactly what let the 2.12G2R lifecycle test
// hooks (CheckTestPreemptOnActiveRoam()/CheckTestLeaveOnActiveRoam()/
// CheckTestDissolveOnActiveRoam()) report a false PASSED whenever a
// natural ARRIVED happened to race ahead of the production stop they were
// actually trying to prove - polling "is the old attempt gone" after the
// fact cannot tell WHY it is gone.
//
// AgentRecord::LastCoordinationStop closes that gap: written synchronously,
// at the exact same moment (the same call, immediately after
// ActionExecutor::StopMoveTo() already ran and immediately before
// ActiveActionState/GroupCoordinationGoalState are reset) by the two real
// production stop sites above - never by a test hook itself, never
// speculatively. A test hook that captured an attempt's own identity
// (SourceGroup, StartedAtMs) before triggering a lifecycle action can then
// wait for LastCoordinationStop to report that EXACT identity with the
// EXPECTED Reason, which is only ever true if that specific production
// stop path genuinely ran for that specific attempt - a natural ARRIVED
// never writes this at all, so it can never be mistaken for one.
enum class CoordinationStopReason : uint8
{
    PreemptedByGoal,
    StoppedByLifecycle
};

inline char const* ToString(CoordinationStopReason reason)
{
    switch (reason)
    {
        case CoordinationStopReason::PreemptedByGoal:    return "PREEMPTED_BY_GOAL";
        case CoordinationStopReason::StoppedByLifecycle: return "STOPPED_BY_LIFECYCLE";
        default:                                         return "UNKNOWN";
    }
}

struct CoordinationStopEvent
{
    CoordinationStopReason Reason;

    // The stopped attempt's own identity - SourceGoal/SourceGroup/
    // StartedAtMs together name exactly the one attempt this event
    // describes, the same identity tuple GroupCoordinationGoal/
    // ActiveActionState themselves already use.
    GoalType SourceGoal;
    GroupId SourceGroup;
    uint64 StartedAtMs = 0;

    // Milestone 2.12G2R P2 fix, round 2 (STATIC review): freshly re-
    // observed (HasOwnMoveToGenerator(), called again right after
    // StopMoveTo() at the exact write site) at the moment this event is
    // recorded - not assumed true just because StopMoveTo() was called.
    // A verifying test hook trusts this recorded fact rather than
    // re-querying the live engine generator itself later, since a
    // legitimate NEW action may have already started its own generator
    // of the same type by the time it polls, which would otherwise be
    // indistinguishable from the OLD, stale one still running.
    bool EngineGeneratorConfirmedStopped = false;

    uint64 StoppedAtMs = 0;
};

#endif // AIWORLD_COORDINATIONSTOPEVENT_H
