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

#include "ActionSystem.h"
#include "ArrivalTolerance.h"
#include <cmath>

namespace
{
    // 2.8D deterministic default - not a tuned gameplay value, chosen
    // mainly so an obviously-wrong destination (whatever AI eventually
    // proposes one) can never send MovePoint() to an arbitrary point on
    // the map. AIWorld.PerceptionSightRange defaults to the same
    // magnitude, which is a reasonable scale for "somewhere this agent
    // could plausibly already be reacting to", not a deliberate coupling
    // between the two.
    constexpr float MaxMoveToRangeYards = 40.0f;

    // Milestone 2.11C: GoToWork/GoHome are a wider bound than the reactive
    // default above - a commute between a persisted HomeLocation/
    // WorkLocation is expected to legitimately be much farther than
    // "somewhere this agent could plausibly already be reacting to" (Pa
    // Maclure's own is already ~58 yards). Still bounded, not unlimited:
    // the destination is curated world data set at the persistence layer
    // (see AgentRecord.h), not an arbitrary point an AI proposal could
    // name, but ValidateMoveTo() must not have to trust that distinction
    // implicitly - this constant is the explicit bound for it either way.
    constexpr float MaxRoutineMoveToRangeYards = 300.0f;

    // Milestone 2.12F2: Regroup's own bound - wider than the default
    // reactive-goal bound above (a member is allowed to drift up to a
    // Loose group's own LeaveRadius, e.g. AIWorld.WolfGroupLeaveRadius,
    // before actually leaving the group - CoalitionMaintenanceSystem's
    // own automatic Leave only fires past that same radius - so a
    // Regroup MOVE_TO must still be able to reach a member from anywhere
    // within it), but deliberately narrower than the routine-commute
    // bound above (a group's own territory is not expected to be nearly
    // as far from a wandering member as a persisted home/work commute
    // can legitimately be).
    constexpr float MaxCoordinationMoveToRangeYards = 100.0f;
}

ActionValidationResult ActionSystem::Validate(ActionRequest const& request, ActionValidationContext const& context) const
{
    if (!context.Materialized)
        return { false, ActionRejectReason::ActorNotMaterialized };

    if (!context.Alive)
        return { false, ActionRejectReason::ActorDead };

    if (!context.ActiveGoalType)
        return { false, ActionRejectReason::NoActiveGoal };

    // The request must honestly describe the actor's actual current goal -
    // not just claim one that happens to support this action type.
    if (request.SourceGoal != *context.ActiveGoalType)
        return { false, ActionRejectReason::GoalMismatch };

    // Not just the same GoalType - the same goal attempt. Irrelevant while
    // request/validate/execute all happen synchronously in one world-thread
    // pass, but this is exactly the identity a future queued/async
    // ActionRequest would need to guard: the actor could otherwise release
    // this goal and activate a new attempt before a stale request gets
    // validated.
    if (request.GoalStartedAtMs != context.ActiveGoalStartedAtMs)
        return { false, ActionRejectReason::GoalMismatch };

    switch (request.Type)
    {
        case ActionType::Flee:
            return ValidateFlee(request, context);
        case ActionType::MoveTo:
            return ValidateMoveTo(request, context);
        case ActionType::Eat:
            return ValidateEat(request, context);
        case ActionType::Work:
            return ValidateWork(request, context);
        case ActionType::Rest:
            return ValidateRest(request, context);
        default:
            return { false, ActionRejectReason::UnsupportedAction };
    }
}

ActionValidationResult ActionSystem::ValidateFlee(ActionRequest const& request, ActionValidationContext const& context) const
{
    // 2.8A/2.8B only know how to validate a Flee request sourced from
    // FleeDanger - deliberately not folded into Validate()'s SourceGoal
    // check, which only proves the request is honest, not that the actual
    // goal is one this system supports.
    if (*context.ActiveGoalType != GoalType::FleeDanger)
        return { false, ActionRejectReason::GoalMismatch };

    // Flee needs somewhere to flee from, and the request must honestly
    // name it - the same pattern as SourceGoal above, applied to the
    // actor's actual current threat victim rather than its goal.
    if (context.FleeSourceGuid.IsEmpty())
        return { false, ActionRejectReason::NoFleeSource };

    if (request.FleeFromGuid != context.FleeSourceGuid)
        return { false, ActionRejectReason::FleeSourceMismatch };

    return { true, ActionRejectReason::None };
}

ActionValidationResult ActionSystem::ValidateMoveTo(ActionRequest const& request, ActionValidationContext const& context) const
{
    if (!request.Destination)
        return { false, ActionRejectReason::NoDestination };

    if (request.Destination->MapId != context.MapId)
        return { false, ActionRejectReason::DestinationMapMismatch };

    if (!std::isfinite(request.Destination->X) || !std::isfinite(request.Destination->Y) || !std::isfinite(request.Destination->Z))
        return { false, ActionRejectReason::DestinationNotFinite };

    // AI must not be able to send MOVE_TO to an arbitrary point on the
    // map - bounded to a fixed range from the actor's own current
    // position, checked after the finite check so a non-finite coordinate
    // is reported as that, not folded into an equally-failing distance
    // comparison. GoToWork/GoHome get the wider routine-commute bound
    // (see MaxRoutineMoveToRangeYards above) - every other GoalType keeps
    // the original reactive-goal bound unchanged.
    float dx = request.Destination->X - context.X;
    float dy = request.Destination->Y - context.Y;
    float dz = request.Destination->Z - context.Z;
    float distanceSq = dx * dx + dy * dy + dz * dz;

    bool isRoutineMove = request.SourceGoal == GoalType::GoToWork || request.SourceGoal == GoalType::GoHome;
    bool isCoordinationMove = request.SourceGoal == GoalType::Regroup;
    float maxRangeYards = isRoutineMove ? MaxRoutineMoveToRangeYards
        : isCoordinationMove ? MaxCoordinationMoveToRangeYards
        : MaxMoveToRangeYards;

    if (distanceSq > maxRangeYards * maxRangeYards)
        return { false, ActionRejectReason::DestinationTooFar };

    // MoveTo may only start from an idle actor. Without this, a new
    // MoveTo added to MOTION_SLOT_ACTIVE could either replace an existing
    // same-priority movement it doesn't own, or queue silently behind a
    // higher-priority one (e.g. an in-progress FLEE) and only actually
    // start once that unrelated movement ends - by which point the
    // destination's map/range validation just performed may no longer be
    // true (the actor could be anywhere after a 20-30s flee).
    if (context.HasActiveMovement)
        return { false, ActionRejectReason::ActorMovementBusy };

    return { true, ActionRejectReason::None };
}

ActionValidationResult ActionSystem::ValidateEat(ActionRequest const& request, ActionValidationContext const& context) const
{
    // Unlike MoveTo, Eat is tied to a specific GoalType - only a GetFood
    // goal ever justifies eating.
    if (*context.ActiveGoalType != GoalType::GetFood)
        return { false, ActionRejectReason::GoalMismatch };

    if (!request.Destination)
        return { false, ActionRejectReason::NoDestination };

    // Milestone 2.8G P2 fix: Eat must be an honest continuation of a
    // MOVE_TO that actually just arrived - not merely a request that
    // happens to claim a Destination/SourceGoal/GoalStartedAtMs able to
    // pass every check below on its own (e.g. the actor's own current
    // position, submitted while GET_FOOD merely happens to be active).
    // ArrivedDestination/ArrivedSourceGoal/ArrivedGoalStartedAtMs are
    // engine-authoritative facts only AIWorldMgr sets, from the completion
    // that actually produced them - never from the request itself. No
    // arrival on record (ArrivedDestination empty) rejects unconditionally.
    if (!context.ArrivedDestination
        || request.Destination->MapId != context.ArrivedDestination->MapId
        || request.Destination->X != context.ArrivedDestination->X
        || request.Destination->Y != context.ArrivedDestination->Y
        || request.Destination->Z != context.ArrivedDestination->Z
        || request.SourceGoal != context.ArrivedSourceGoal
        || request.GoalStartedAtMs != context.ArrivedGoalStartedAtMs)
        return { false, ActionRejectReason::EatContinuationMismatch };

    if (request.Destination->MapId != context.MapId)
        return { false, ActionRejectReason::DestinationMapMismatch };

    if (!std::isfinite(request.Destination->X) || !std::isfinite(request.Destination->Y) || !std::isfinite(request.Destination->Z))
        return { false, ActionRejectReason::DestinationNotFinite };

    // The same tolerance MOVE_TO arrival uses (ArrivalTolerance.h) - the
    // actor must actually be standing at the food target, not just
    // somewhere on the same map.
    if (!IsWithinArrivalTolerance(*request.Destination, context.X, context.Y, context.Z))
        return { false, ActionRejectReason::DestinationTooFar };

    // Checked last: if danger appears the same tick the actor arrives at a
    // food target, it must not start eating a moment before an emergency
    // FLEE_DANGER takes over.
    if (context.InCombat)
        return { false, ActionRejectReason::ActorInCombat };

    return { true, ActionRejectReason::None };
}

ActionValidationResult ActionSystem::ValidateWork(ActionRequest const& request, ActionValidationContext const& context) const
{
    // Unlike MoveTo, Work is tied to a specific GoalType - only a
    // GoToWork routine goal ever justifies it, the same rule ValidateEat()
    // already applies to GetFood/Eat.
    if (*context.ActiveGoalType != GoalType::GoToWork)
        return { false, ActionRejectReason::GoalMismatch };

    // Milestone 2.11E1 P3 fix: independent authoritative check against
    // AgentRecord::RoutineActivityState itself - see
    // ActionValidationContext::ExpectedRoutineActivity for why this is not
    // just the generic ActiveGoalType/ActiveGoalStartedAtMs check above.
    if (context.ExpectedRoutineActivity != RoutineActivityType::Work
        || request.GoalStartedAtMs != context.RoutineActivityStartedAtMs)
        return { false, ActionRejectReason::RoutineActivityMismatch };

    return ValidateAtRoutineTarget(request, context);
}

ActionValidationResult ActionSystem::ValidateRest(ActionRequest const& request, ActionValidationContext const& context) const
{
    if (*context.ActiveGoalType != GoalType::GoHome)
        return { false, ActionRejectReason::GoalMismatch };

    if (context.ExpectedRoutineActivity != RoutineActivityType::Rest
        || request.GoalStartedAtMs != context.RoutineActivityStartedAtMs)
        return { false, ActionRejectReason::RoutineActivityMismatch };

    return ValidateAtRoutineTarget(request, context);
}

ActionValidationResult ActionSystem::ValidateAtRoutineTarget(ActionRequest const& request, ActionValidationContext const& context) const
{
    if (!request.Destination)
        return { false, ActionRejectReason::NoDestination };

    if (request.Destination->MapId != context.MapId)
        return { false, ActionRejectReason::DestinationMapMismatch };

    if (!std::isfinite(request.Destination->X) || !std::isfinite(request.Destination->Y) || !std::isfinite(request.Destination->Z))
        return { false, ActionRejectReason::DestinationNotFinite };

    // Unlike ValidateEat(), this is a live "is the actor still there right
    // now" check against the actor's actual current position - not a
    // one-time continuation of a specific MOVE_TO arrival event. Work/Rest
    // is an ongoing state, re-derived fresh every tick by
    // RoutineActivitySystem (see AIWorldMgr::UpdateNeeds()'s 2.11D
    // activity block), so there is no "Arrived*" snapshot to check against
    // the way Eat has one.
    if (!IsWithinArrivalTolerance(*request.Destination, context.X, context.Y, context.Z))
        return { false, ActionRejectReason::DestinationTooFar };

    // Same rule ValidateMoveTo() enforces the other direction (may only
    // start moving from an idle actor) - here it means the actor must
    // actually be standing still, not merely within tolerance of the
    // target while something is still physically moving it.
    if (context.HasActiveMovement)
        return { false, ActionRejectReason::ActorMovementBusy };

    return { true, ActionRejectReason::None };
}
