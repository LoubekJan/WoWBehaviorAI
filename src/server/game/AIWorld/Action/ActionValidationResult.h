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

#ifndef AIWORLD_ACTIONVALIDATIONRESULT_H
#define AIWORLD_ACTIONVALIDATIONRESULT_H

#include "Define.h"

enum class ActionRejectReason : uint8
{
    None,
    // Milestone 2.12F4A: the mandatory authoritative ControlMode gate -
    // see ActionSystem::Validate()'s own comment. Checked first, before
    // ActorNotMaterialized/ActorDead/anything else, since an ObserveOnly
    // agent is rejected regardless of whether it is materialized/alive/
    // has an active goal at all.
    ControlModeNotAllowed,
    ActorNotMaterialized,
    ActorDead,
    NoActiveGoal,
    GoalMismatch,
    UnsupportedAction,
    NoFleeSource,
    FleeSourceMismatch,
    NoDestination,
    DestinationMapMismatch,
    DestinationNotFinite,
    DestinationTooFar,
    ActorMovementBusy,
    ActorInCombat,
    // Milestone 2.8G P2 fix: an Eat request's Destination/SourceGoal/
    // GoalStartedAtMs did not match ActionValidationContext::
    // ArrivedDestination/ArrivedSourceGoal/ArrivedGoalStartedAtMs - either
    // no MOVE_TO just arrived at all, or the request claims a different
    // continuation than the one that actually did.
    EatContinuationMismatch,
    // Milestone 2.11E1 P3 fix: a Work/Rest request's SourceGoal/
    // GoalStartedAtMs did not match ActionValidationContext::
    // ExpectedRoutineActivity/RoutineActivityStartedAtMs - the same
    // "independent authoritative fields, not just the generic ActiveGoal
    // pair" pattern EatContinuationMismatch already uses.
    RoutineActivityMismatch,
    // Milestone 2.12G3C1: a HUNT approach (MoveTo, SourceGoal=Hunt) whose
    // ActionRequest::Target is missing, or names an empty GUID or a zero
    // Entry - none of which can ever honestly name a real creature target.
    // See ActionSystem::ValidateMoveTo()'s own HUNT-specific block.
    TargetMissing,
    // ActionValidationContext::TargetResolved is false - the caller could
    // not resolve any live target for this request at all.
    TargetNotResolved,
    // The resolved target exists but ActionValidationContext::TargetAlive
    // is false.
    TargetDead,
    // The resolved target exists and is alive, but
    // ActionValidationContext::TargetAttackable is false for this actor.
    TargetNotAttackable,
    // ActionRequest::Target->Guid does not match
    // ActionValidationContext::TargetGuid - the request does not honestly
    // name the same target the caller actually resolved.
    TargetIdentityMismatch,
    // ActionRequest::Target->Entry does not match
    // ActionValidationContext::TargetEntry.
    TargetEntryMismatch,
    // Either the resolved target is not on the actor's own current map
    // (ActionValidationContext::TargetMapId != MapId), or the request's
    // own Destination is not on the target's map - a HUNT approach can
    // only ever be validated against a target actually reachable from the
    // actor's own current map.
    TargetMapMismatch,
    // Either ActionValidationContext::TargetX/Y/Z is not finite, or the
    // request's own Destination does not match the target's actual current
    // position - a HUNT approach's Destination must be provably where the
    // target actually is right now, not merely a geometrically-valid
    // MoveTo destination that happens to be nearby.
    TargetPositionMismatch
};

inline char const* ToString(ActionRejectReason reason)
{
    switch (reason)
    {
        case ActionRejectReason::None:                   return "NONE";
        case ActionRejectReason::ControlModeNotAllowed:  return "CONTROL_MODE_NOT_ALLOWED";
        case ActionRejectReason::ActorNotMaterialized:   return "ACTOR_NOT_MATERIALIZED";
        case ActionRejectReason::ActorDead:              return "ACTOR_DEAD";
        case ActionRejectReason::NoActiveGoal:           return "NO_ACTIVE_GOAL";
        case ActionRejectReason::GoalMismatch:           return "GOAL_MISMATCH";
        case ActionRejectReason::UnsupportedAction:      return "UNSUPPORTED_ACTION";
        case ActionRejectReason::NoFleeSource:           return "NO_FLEE_SOURCE";
        case ActionRejectReason::FleeSourceMismatch:     return "FLEE_SOURCE_MISMATCH";
        case ActionRejectReason::NoDestination:          return "NO_DESTINATION";
        case ActionRejectReason::DestinationMapMismatch: return "DESTINATION_MAP_MISMATCH";
        case ActionRejectReason::DestinationNotFinite:   return "DESTINATION_NOT_FINITE";
        case ActionRejectReason::DestinationTooFar:      return "DESTINATION_TOO_FAR";
        case ActionRejectReason::ActorMovementBusy:      return "ACTOR_MOVEMENT_BUSY";
        case ActionRejectReason::ActorInCombat:          return "ACTOR_IN_COMBAT";
        case ActionRejectReason::EatContinuationMismatch: return "EAT_CONTINUATION_MISMATCH";
        case ActionRejectReason::RoutineActivityMismatch: return "ROUTINE_ACTIVITY_MISMATCH";
        case ActionRejectReason::TargetMissing:           return "TARGET_MISSING";
        case ActionRejectReason::TargetNotResolved:       return "TARGET_NOT_RESOLVED";
        case ActionRejectReason::TargetDead:              return "TARGET_DEAD";
        case ActionRejectReason::TargetNotAttackable:     return "TARGET_NOT_ATTACKABLE";
        case ActionRejectReason::TargetIdentityMismatch:  return "TARGET_IDENTITY_MISMATCH";
        case ActionRejectReason::TargetEntryMismatch:     return "TARGET_ENTRY_MISMATCH";
        case ActionRejectReason::TargetMapMismatch:       return "TARGET_MAP_MISMATCH";
        case ActionRejectReason::TargetPositionMismatch:  return "TARGET_POSITION_MISMATCH";
        default:                                         return "UNKNOWN";
    }
}

// Milestone 2.8A/2.8B/2.8D: ActionSystem::Validate()'s verdict - never
// itself a permission to mutate the world, just a pure ALLOWED/REJECTED
// judgment. Only ActionExecutor, and only on Allowed == true, actually
// touches TrinityCore.
struct ActionValidationResult
{
    bool Allowed = false;
    ActionRejectReason Reason = ActionRejectReason::None;
};

#endif // AIWORLD_ACTIONVALIDATIONRESULT_H
