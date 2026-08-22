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
    ActorNotMaterialized,
    ActorDead,
    NoActiveGoal,
    GoalMismatch,
    UnsupportedAction
};

inline char const* ToString(ActionRejectReason reason)
{
    switch (reason)
    {
        case ActionRejectReason::None:                 return "NONE";
        case ActionRejectReason::ActorNotMaterialized: return "ACTOR_NOT_MATERIALIZED";
        case ActionRejectReason::ActorDead:            return "ACTOR_DEAD";
        case ActionRejectReason::NoActiveGoal:         return "NO_ACTIVE_GOAL";
        case ActionRejectReason::GoalMismatch:         return "GOAL_MISMATCH";
        case ActionRejectReason::UnsupportedAction:    return "UNSUPPORTED_ACTION";
        default:                                       return "UNKNOWN";
    }
}

// Milestone 2.8A: ActionSystem::Validate()'s verdict - never itself a
// permission to mutate the world, just a pure ALLOWED/REJECTED judgment.
// 2.8A never acts on Allowed == true; that's 2.8B.
struct ActionValidationResult
{
    bool Allowed = false;
    ActionRejectReason Reason = ActionRejectReason::None;
};

#endif // AIWORLD_ACTIONVALIDATIONRESULT_H
