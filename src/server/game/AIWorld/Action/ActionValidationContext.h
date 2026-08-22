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
#include "ObjectGuid.h"
#include <optional>

// Milestone 2.8A/2.8B/2.8D: the world-thread facts ActionSystem::Validate()
// is allowed to see, as plain values - AIWorldMgr resolves Materialized/
// Alive/the actor's current ActiveGoal/current threat victim/current
// position itself (it already has the live Creature and AgentRecord at
// the call site) and hands over only this. ActionSystem never sees a
// Creature*, AgentRecord*, Unit*, Map*, or the registry.
struct ActionValidationContext
{
    bool Materialized = false;
    bool Alive = false;

    std::optional<GoalType> ActiveGoalType;
    uint64 ActiveGoalStartedAtMs = 0;

    // Milestone 2.8B: the actor's actual current threat victim GUID
    // (ThreatManager::GetCurrentVictim()), empty if it has none. Compared
    // against ActionRequest::FleeFromGuid for a Flee request - reality,
    // not the request's claim.
    ObjectGuid FleeSourceGuid;

    // Milestone 2.8D: the actor's actual current position, for MoveTo's
    // map-match and max-range checks - reality, not wherever the request
    // claims the actor is.
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;

    // Milestone 2.8D P2 fix: whether the actor's MOTION_SLOT_ACTIVE
    // already has a movement generator running (GetCurrentMovementGenerator
    // (MOTION_SLOT_ACTIVE) != nullptr) - e.g. an in-progress FLEE.
    // ValidateMoveTo() rejects rather than let a new MoveTo either replace
    // a movement it doesn't own or queue silently behind one and only
    // actually start once that unrelated movement ends, by which point the
    // destination's range/map validation may no longer be true. MoveTo may
    // only start from an idle actor.
    bool HasActiveMovement = false;
};

#endif // AIWORLD_ACTIONVALIDATIONCONTEXT_H
