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

#ifndef AIWORLD_ACTIONSYSTEM_H
#define AIWORLD_ACTIONSYSTEM_H

#include "ActionRequest.h"
#include "ActionValidationContext.h"
#include "ActionValidationResult.h"
#include "Define.h"

// Milestone 2.8A/2.8B/2.8D/2.8G: the safety boundary between "AI proposes" and
// "TrinityCore executes" - AI proposes (ActionRequest), ActionSystem
// validates (this class), ActionExecutor executes only on ALLOWED.
// Validate() is a pure value transform: no Creature*, AgentRecord*, Map*,
// Unit*, registry, or DB, and it never mutates anything - it only judges
// whether a request is currently ALLOWED.
class TC_GAME_API ActionSystem
{
    public:
        // Checks common to every ActionType, in this fixed order:
        // ControlMode first (2.12F4A - the mandatory authoritative safety
        // gate, rejects anything other than AIWorldControlled unconditionally,
        // see this method's own .cpp comment), then actor state
        // (materialized, alive), then whether the actor even has a goal to
        // act on, then whether the request honestly describes that goal -
        // both its GoalType (SourceGoal) and the specific goal attempt
        // (GoalStartedAtMs). Only then does it dispatch to the
        // per-ActionType validation below (or reject UnsupportedAction for
        // anything else).
        ActionValidationResult Validate(ActionRequest const& request, ActionValidationContext const& context) const;

        // Milestone 2.12F2 P3 fix (STATIC review): exposes the exact same
        // range bound ValidateMoveTo() enforces for a GoalType::Regroup
        // request (see that method's own comment) - so a caller building an
        // AgentGroupCoordinationProfile (or clamping a maintenance profile's
        // own LeaveRadius against it, since a member may legitimately sit
        // anywhere up to LeaveRadius from group territory while still a
        // member) never has to duplicate this number as a second,
        // independently-maintained magic constant. An earlier version had
        // no such accessor - a coordination profile radius left unbound
        // against it (e.g. AIWorld.WolfGroupLeaveRadius set above 100 with
        // no warning) would let AgentGroupIntentSystem/AgentGroupIntentProjector
        // keep proposing a Regroup that ValidateMoveTo() then rejects as
        // DestinationTooFar every single pass, for a member that is
        // otherwise a perfectly legitimate one. Named distinctly from the
        // internal MaxCoordinationMoveToRangeYards constant it returns
        // (rather than reusing that exact name for this static method) so
        // there is no unqualified-lookup ambiguity between the two inside
        // ActionSystem.cpp's own implementation.
        static float CoordinationMoveToRangeYards();

    private:
        // Flee-specific, run only once the common checks above already
        // passed: the actor's goal must actually be FleeDanger (not just
        // any goal), the actor must actually have a threat victim, and the
        // request must honestly name it.
        ActionValidationResult ValidateFlee(ActionRequest const& request, ActionValidationContext const& context) const;

        // MoveTo-specific, run only once the common checks above already
        // passed. Deliberately not tied to a specific GoalType the way
        // ValidateFlee() is to FleeDanger - MoveTo isn't semantically
        // owned by one goal, any active goal is enough justification (the
        // common checks above already proved there is one and the request
        // honestly names it). Checks the destination exists, is on the
        // actor's own map, has finite coordinates, and is within a bounded
        // range - see ActionSystem.cpp for the exact distances (GoToWork/
        // GoHome get the widest, routine-commute bound; Regroup/Roam/Hunt -
        // 2.12F2/2.12G2/2.12G3C1 - share a narrower-than-routine-but-wider-
        // than-default bound, wide enough to reach a member from anywhere
        // within a Loose group's own LeaveRadius; every other GoalType
        // keeps the original reactive-goal bound). Milestone 2.12G3C1: for
        // SourceGoal == GoalType::Hunt specifically, this geometric check
        // alone is not enough - see ValidateHuntTarget() below, invoked
        // from within this method, for the additional target-identity
        // requirements a HUNT approach must also satisfy.
        ActionValidationResult ValidateMoveTo(ActionRequest const& request, ActionValidationContext const& context) const;

        // Milestone 2.12G3C1: invoked from ValidateMoveTo() only when
        // request.SourceGoal == GoalType::Hunt, after that method's own
        // generic Destination existence/map/finite checks already passed
        // but before its range/ActorMovementBusy checks run. Proves the
        // request's claimed target (ActionRequest::Target) is internally
        // honest - not just present, but a real creature GUID whose own
        // embedded entry actually matches its claimed Entry (2.12G3C1 P2
        // fix, STATIC review: closes the same GUID/entry-binding gap
        // 2.12G3B's own review already found in HuntTargetProvenance) -
        // and honestly agrees with what the caller actually resolved
        // (ActionValidationContext::TargetResolved/TargetAlive/
        // TargetAttackable/TargetGuid/TargetEntry/TargetMapId/X/Y/Z), and
        // that the request's own Destination is provably that target's
        // actual current position, not merely a geometrically-plausible
        // point nearby - see ActionRequest::Target's own comment and
        // ActionRejectReason's TargetMissing..TargetPositionMismatch
        // values for the exact rule set. A HUNT approach must never be
        // ALLOWED on Destination geometry alone.
        ActionValidationResult ValidateHuntTarget(ActionRequest const& request, ActionValidationContext const& context) const;

        // Milestone 2.12G3D: Attack-specific, run only once the common
        // checks above already passed. Tied to a specific GoalType the
        // same way ValidateEat() is to GetFood - only a Hunt goal ever
        // justifies attacking. Shares most of its target-identity
        // requirements with ValidateHuntTarget() above (the request's
        // claimed Target must provably be a real creature whose own
        // embedded entry matches, and must honestly agree with what the
        // caller actually resolved into context.Target*), but deliberately
        // does NOT check request.Destination at all - ATTACK carries no
        // Destination (see ActionRequest::Destination's own "empty for
        // every other ActionType" convention), and never compares the
        // target's position against a stale MoveTo-style snapshot: the
        // target may keep moving once combat starts, and TrinityCore's own
        // chase movement (see ActionExecutor::ExecuteAttack()) is what
        // keeps up with it, not a re-validated Destination. Additionally
        // checks the actor is not already engaged with a DIFFERENT live
        // target - see ActionRejectReason::ActorEngagedWithDifferentTarget.
        ActionValidationResult ValidateAttack(ActionRequest const& request, ActionValidationContext const& context) const;

        // Milestone 2.8G/2.8G P2 fix: Eat-specific, run only once the
        // common checks above already passed. Unlike MoveTo, Eat is tied
        // to a specific GoalType - only GetFood ever justifies eating.
        // Checks the destination exists, matches
        // ActionValidationContext::ArrivedDestination/ArrivedSourceGoal/
        // ArrivedGoalStartedAtMs exactly (the request must be an honest
        // continuation of a MOVE_TO that actually just arrived, not merely
        // a self-consistent claim), is on the actor's own map, has finite
        // coordinates, is within ArrivalToleranceYards of the actor's
        // actual position (the same tolerance MOVE_TO arrival uses - see
        // ArrivalTolerance.h), and that the actor isn't in combat: if
        // danger appears the same tick the actor arrives at a food target,
        // it must not start eating a moment before an emergency
        // FLEE_DANGER takes over.
        ActionValidationResult ValidateEat(ActionRequest const& request, ActionValidationContext const& context) const;

        // Milestone 2.11E1/2.11E1 P3 fix: Work/Rest-specific, run only once
        // the common checks above already passed. Each is tied to a
        // specific GoalType the same way ValidateEat() is to GetFood - Work
        // only ever justified by GoToWork, Rest only ever by GoHome - and
        // each also cross-checks the request against
        // ActionValidationContext::ExpectedRoutineActivity/
        // RoutineActivityStartedAtMs, an authoritative pair independent of
        // the generic ActiveGoalType/ActiveGoalStartedAtMs check (see that
        // field's own comment for why). Both then defer to the shared
        // ValidateAtRoutineTarget() below, since what makes either one
        // currently allowed is otherwise identical: the actor must
        // actually be standing at the request's own Destination right now
        // (live position, not a stale arrival snapshot the way Eat uses -
        // Work/Rest are an ongoing "still there" check, not a one-time
        // continuation), and not moving.
        ActionValidationResult ValidateWork(ActionRequest const& request, ActionValidationContext const& context) const;
        ActionValidationResult ValidateRest(ActionRequest const& request, ActionValidationContext const& context) const;
        ActionValidationResult ValidateAtRoutineTarget(ActionRequest const& request, ActionValidationContext const& context) const;
};

#endif // AIWORLD_ACTIONSYSTEM_H
