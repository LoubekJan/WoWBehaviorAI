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

// Milestone 2.8A/2.8B/2.8D: the safety boundary between "AI proposes" and
// "TrinityCore executes" - AI proposes (ActionRequest), ActionSystem
// validates (this class), ActionExecutor executes only on ALLOWED.
// Validate() is a pure value transform: no Creature*, AgentRecord*, Map*,
// Unit*, registry, or DB, and it never mutates anything - it only judges
// whether a request is currently ALLOWED.
class TC_GAME_API ActionSystem
{
    public:
        // Checks common to every ActionType, in this fixed order: actor
        // state first (materialized, alive), then whether the actor even
        // has a goal to act on, then whether the request honestly
        // describes that goal - both its GoalType (SourceGoal) and the
        // specific goal attempt (GoalStartedAtMs). Only then does it
        // dispatch to the per-ActionType validation below (or reject
        // UnsupportedAction for anything else).
        ActionValidationResult Validate(ActionRequest const& request, ActionValidationContext const& context) const;

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
        // range - see ActionSystem.cpp for the exact distance.
        ActionValidationResult ValidateMoveTo(ActionRequest const& request, ActionValidationContext const& context) const;
};

#endif // AIWORLD_ACTIONSYSTEM_H
