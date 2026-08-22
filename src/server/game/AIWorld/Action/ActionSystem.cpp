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
    // pass (2.8B), but this is exactly the identity a future queued/async
    // ActionRequest would need to guard: the actor could otherwise release
    // this goal and activate a new FleeDanger attempt before a stale
    // request gets validated.
    if (request.GoalStartedAtMs != context.ActiveGoalStartedAtMs)
        return { false, ActionRejectReason::GoalMismatch };

    if (request.Type != ActionType::Flee)
        return { false, ActionRejectReason::UnsupportedAction };

    // 2.8A/2.8B only know how to validate a Flee request sourced from
    // FleeDanger - deliberately not folded into the SourceGoal check
    // above, which only proves the request is honest, not that the actual
    // goal is one this system supports.
    if (*context.ActiveGoalType != GoalType::FleeDanger)
        return { false, ActionRejectReason::GoalMismatch };

    // Milestone 2.8B: Flee needs somewhere to flee from, and the request
    // must honestly name it - the same pattern as SourceGoal above, applied
    // to the actor's actual current threat victim rather than its goal.
    if (context.FleeSourceGuid.IsEmpty())
        return { false, ActionRejectReason::NoFleeSource };

    if (request.FleeFromGuid != context.FleeSourceGuid)
        return { false, ActionRejectReason::FleeSourceMismatch };

    return { true, ActionRejectReason::None };
}
