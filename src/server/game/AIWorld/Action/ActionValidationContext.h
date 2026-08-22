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

#ifndef AIWORLD_ACTIONVALIDATIONCONTEXT_H
#define AIWORLD_ACTIONVALIDATIONCONTEXT_H

#include "Define.h"
#include "Goal/GoalType.h"
#include <optional>

// Milestone 2.8A: the world-thread facts ActionSystem::Validate() is
// allowed to see, as plain values - AIWorldMgr resolves Materialized/
// Alive/the actor's current ActiveGoal itself (it already has the live
// Creature and AgentRecord at the call site) and hands over only this.
// ActionSystem never sees a Creature*, AgentRecord*, or the registry.
struct ActionValidationContext
{
    bool Materialized = false;
    bool Alive = false;

    std::optional<GoalType> ActiveGoalType;
    uint64 ActiveGoalStartedAtMs = 0;
};

#endif // AIWORLD_ACTIONVALIDATIONCONTEXT_H
