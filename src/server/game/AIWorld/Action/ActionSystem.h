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

// Milestone 2.8A: the safety boundary between "AI proposes" and
// "TrinityCore executes" - AI proposes (ActionRequest), ActionSystem
// validates (this class), TrinityCore would eventually execute (2.8B).
// Validate() is a pure value transform: no Creature*, AgentRecord*, Map*,
// registry, or DB, and it never mutates anything - it only judges whether
// a request is currently ALLOWED. Nothing in 2.8A acts on an Allowed
// result yet.
class TC_GAME_API ActionSystem
{
    public:
        // Checked in this fixed order - actor state first (materialized,
        // alive), then whether the actor even has a goal to act on, then
        // whether the request honestly describes that goal, then whether
        // the requested action type is supported at all, then whether the
        // actual goal is one 2.8A knows how to validate an action for.
        ActionValidationResult Validate(ActionRequest const& request, ActionValidationContext const& context) const;
};

#endif // AIWORLD_ACTIONSYSTEM_H
