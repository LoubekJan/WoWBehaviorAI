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
    RoutineActivityMismatch
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
