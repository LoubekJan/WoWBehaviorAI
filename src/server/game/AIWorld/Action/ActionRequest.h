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

#ifndef AIWORLD_ACTIONREQUEST_H
#define AIWORLD_ACTIONREQUEST_H

#include "Agent/AgentId.h"
#include "ActionType.h"
#include "Define.h"
#include "Goal/GoalType.h"
#include "ObjectGuid.h"

// Milestone 2.8A/2.8B: what an agent's ActiveGoal proposes to do - an
// intent, not a command. AIWorldMgr builds this only on ACTIVATED/
// INTERRUPTED into GoalType::FleeDanger, never every tick.
// ActionSystem::Validate() decides ALLOWED/REJECTED; only on ALLOWED does
// ActionExecutor (2.8B) actually execute it against TrinityCore. Pure
// value: no Creature*, Player*, Map*, or WorldObject* - FleeFromGuid is a
// value identity (ObjectGuid), not a live reference.
struct ActionRequest
{
    AgentId Actor;
    ActionType Type = ActionType::Flee;

    // What ActiveGoal this request claims to come from - Validate() checks
    // this against the actor's actual current goal AND the specific goal
    // attempt (GoalMismatch) rather than trusting the caller. Not yet
    // exploitable in 2.8B, where the request is built and validated
    // synchronously in the same world-thread pass with no queue, but this
    // is exactly the identity a future queued/async ActionRequest would
    // need.
    GoalType SourceGoal = GoalType::FleeDanger;
    uint64 GoalStartedAtMs = 0;

    // Milestone 2.8B: who this Flee request claims to be fleeing from -
    // resolved by AIWorldMgr from the actor's current threat victim at
    // request-build time. Validate() checks this against the actor's
    // actual current threat victim (FleeSourceMismatch), the same
    // honesty-not-trust pattern as SourceGoal above. Only meaningful for
    // ActionType::Flee.
    ObjectGuid FleeFromGuid;
};

#endif // AIWORLD_ACTIONREQUEST_H
