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
#include <optional>

// Milestone 2.12G2R P2 fix, round 2 (STATIC review): a group-coordination-
// owned MOVE_TO (GroupCoordinationGoal.h) can stop for genuinely different
// production reasons - AIWorldMgr::UpdateNeeds()'s own
// COORDINATION_PREEMPTED_BY_GOAL block (a higher-priority individual goal
// took the action slot back), AIWorldMgr::StopInFlightGroupCoordination()'s
// own COORDINATION_STOPPED_BY_LIFECYCLE path (a confirmed Leave/Dissolve
// stopped it), or that SAME shared method's COORDINATION_STOPPED_BY_MEMBERSHIP_AMBIGUITY
// path (2.12G2R P2 fix, round 3, STATIC review - a confirmed Join made
// membership newly ambiguous; see ReconcileGroupCoordinationForMember())
// - and, separately, it can also simply ARRIVE at its own destination
// (AIWorldMgr::HandleActionCompletion()), which is not a "stop" in this
// sense at all. From OUTSIDE AgentRecord, all of these end states look
// identical: ActiveActionState/GroupCoordinationGoalState both gone. That
// ambiguity is exactly what let the 2.12G2R lifecycle test hooks
// (CheckTestPreemptOnActiveRoam()/CheckTestLeaveOnActiveRoam()/
// CheckTestDissolveOnActiveRoam()) report a false PASSED whenever a
// natural ARRIVED happened to race ahead of the production stop they were
// actually trying to prove, or whenever a membership-ambiguity stop
// happened to be mistaken for the specific lifecycle stop a leave/dissolve
// hook was waiting for - polling "is the old attempt gone" after the fact
// cannot tell WHY it is gone.
//
// AgentRecord::LastCoordinationStop closes that gap: written synchronously,
// at the exact same moment (the same call, immediately after
// ActionExecutor::StopMoveTo() already ran and immediately before
// ActiveActionState/GroupCoordinationGoalState are reset) by the real
// production stop sites above - never by a test hook itself, never
// speculatively. A test hook that captured an attempt's own identity
// (SourceGroup, StartedAtMs) before triggering a lifecycle action can then
// wait for LastCoordinationStop to report that EXACT identity with the
// EXPECTED Reason, which is only ever true if that specific production
// stop path genuinely ran for that specific attempt - a natural ARRIVED
// never writes this at all, so it can never be mistaken for one, and a
// membership-ambiguity stop is tagged with its own distinct Reason, so it
// can never be mistaken for a Leave/Dissolve-sourced one either.
enum class CoordinationStopReason : uint8
{
    PreemptedByGoal,
    StoppedByLifecycle,
    StoppedByMembershipAmbiguity
};

inline char const* ToString(CoordinationStopReason reason)
{
    switch (reason)
    {
        case CoordinationStopReason::PreemptedByGoal:             return "PREEMPTED_BY_GOAL";
        case CoordinationStopReason::StoppedByLifecycle:          return "STOPPED_BY_LIFECYCLE";
        case CoordinationStopReason::StoppedByMembershipAmbiguity: return "STOPPED_BY_MEMBERSHIP_AMBIGUITY";
        default:                                                  return "UNKNOWN";
    }
}

// Milestone 2.12G2R P2 fix, round 3 (STATIC review): which higher-priority
// owner actually preempted a coordination-owned MOVE_TO - only meaningful
// when CoordinationStopEvent::Reason == PreemptedByGoal. UpdateNeeds()'s
// own COORDINATION_PREEMPTED_BY_GOAL block fires on EITHER
// AgentRecord::ActiveGoalState or ::RoutineGoalState being set (see its
// own comment) - this records honestly which one it actually was,
// captured synchronously at the exact stop moment, so a verifying test
// hook never has to re-derive "what preempted this" from AgentRecord's
// own CURRENT (possibly already-moved-on) state.
enum class CoordinationPreemptingOwner : uint8
{
    ActiveGoal,
    RoutineGoal
};

inline char const* ToString(CoordinationPreemptingOwner owner)
{
    switch (owner)
    {
        case CoordinationPreemptingOwner::ActiveGoal:  return "ACTIVE_GOAL";
        case CoordinationPreemptingOwner::RoutineGoal: return "ROUTINE_GOAL";
        default:                                       return "UNKNOWN";
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

    // Milestone 2.12G2R P2 fix, round 3 (STATIC review): two SEPARATE
    // freshly-observed facts, not one - an earlier version only checked
    // HasOwnMoveToGenerator() AFTER calling StopMoveTo(), which reads
    // true ("confirmed stopped") just as readily when nothing was running
    // in the first place (e.g. the attempt had already arrived naturally
    // moments before this stop path even ran, and StopMoveTo() found
    // nothing to remove) as when a genuinely in-flight movement was just
    // interrupted - the two cases are NOT the same claim, and only the
    // second one is actual evidence this stop path did meaningful work.
    // WasRunningBeforeStop is observed BEFORE StopMoveTo() is called;
    // ConfirmedStoppedAfterStop is observed again right after. Both
    // default false (unverified, not "assumed true") - if the record's
    // own Creature cannot be resolved at all (not Materialized, or
    // ResolveLiveCreature() fails), NEITHER can be honestly confirmed one
    // way or the other, and both are deliberately left false rather than
    // defaulted to a claimed success.
    bool EngineGeneratorWasRunningBeforeStop = false;
    bool EngineGeneratorConfirmedStoppedAfterStop = false;

    // Milestone 2.12G2R P2 fix, round 3 (STATIC review): only set when
    // Reason == PreemptedByGoal - see CoordinationPreemptingOwner's own
    // comment for why this must be captured HERE, synchronously, rather
    // than a verifying test hook re-deriving "what is ActiveGoalState
    // right now" on a later poll (which could already have changed -
    // the preempting goal itself may have already finished, or a
    // completely different one may have taken its place, by the time
    // anything else gets a chance to look).
    std::optional<CoordinationPreemptingOwner> PreemptingOwner;
    std::optional<GoalType> PreemptingGoal;

    uint64 StoppedAtMs = 0;
};

#endif // AIWORLD_COORDINATIONSTOPEVENT_H
