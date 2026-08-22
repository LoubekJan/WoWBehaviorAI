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

#ifndef AIWORLD_AGENTRECORD_H
#define AIWORLD_AGENTRECORD_H

#include "AgentId.h"
#include "AgentType.h"
#include "Define.h"
#include "Goal/ActiveGoal.h"
#include "Needs/NeedsState.h"
#include "Needs/NeedsThresholdState.h"
#include "ObjectGuid.h"
#include <optional>

// One persistent agent's registry-owned state. SpawnId/MapId is the stable
// binding to a TrinityCore creature spawn; RuntimeGuid is only the current
// Creature this agent happens to be bound to right now (meaningful only
// while WorldState == Materialized) - never the agent's identity, and never
// a substitute for AgentId. Deliberately holds no Creature*/Map*.
struct AgentRecord
{
    AgentId Id;
    AgentType Type = AgentType::Civilian;

    uint32 MapId = 0;
    uint64 SpawnId = 0;

    ObjectGuid RuntimeGuid;
    AgentWorldState WorldState = AgentWorldState::Abstract;

    uint64 SnapshotSequence = 0;

    // Milestone 2.6A: owned here, not by NeedsSystem - AgentRecord is
    // already the registry-owned home for an agent's runtime state, and
    // Needs must survive Creature unload/reload the same way everything
    // else here does. Frozen (not updated) while WorldState == Abstract.
    NeedsState Needs;

    // Milestone 2.6C: edge-trigger latch for NeedsThresholdEvent, owned
    // alongside Needs for the same reason - must survive Creature
    // unload/reload so a threshold doesn't spuriously re-fire on the next
    // materialize.
    NeedsThresholdState NeedsThresholds;

    // Milestone 2.7B1: the agent's single currently-selected goal, or
    // empty if none is active. Owned here for the same reason as Needs -
    // must survive Creature unload/reload within this process. Not
    // persisted across restart yet.
    std::optional<ActiveGoal> ActiveGoalState;
};

#endif // AIWORLD_AGENTRECORD_H
