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

// Milestone 2.8A: what an agent's ActiveGoal proposes to do - an intent,
// not a command. AIWorldMgr builds this only on ACTIVATED/INTERRUPTED into
// GoalType::FleeDanger, never every tick. ActionSystem::Validate() decides
// ALLOWED/REJECTED; nothing yet actually executes it against TrinityCore
// (that's 2.8B). Pure value: no Creature*, Player*, Map*, or WorldObject*.
struct ActionRequest
{
    AgentId Actor;
    ActionType Type = ActionType::Flee;

    // What ActiveGoal this request claims to come from - Validate() checks
    // this against the actor's actual current goal (GoalMismatch) rather
    // than trusting the caller.
    GoalType SourceGoal = GoalType::FleeDanger;
    uint64 GoalStartedAtMs = 0;
};

#endif // AIWORLD_ACTIONREQUEST_H
