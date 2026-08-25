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

#ifndef AIWORLD_AGENTGROUPSTATE_H
#define AIWORLD_AGENTGROUPSTATE_H

#include "AgentGroupKind.h"
#include "Define.h"

// Milestone 2.12A/2.12B/2.12C: pure, persisted value-state for an
// AgentType::AgentGroup - a social layer over independent member agents
// (see AgentRecord::GroupMembers/AgentGroupMembership.h), never an
// aggregate that stands in for them. Renamed from CreatureGroupState: the
// old name and its own Population field modeled "several creatures merged
// into one agent", exactly the model this milestone moves away from -
// member count is now AgentRecord::GroupMembers.size(), never stored here
// as its own source of truth.
//
// A single Territory center point is pure identity, unchanged by any
// simulation that exists yet. Hunger/Resources (2.12B) are the first two
// fields an actual background simulation drifts, on the same 0.0-1.0
// normalized "pressure"/"level" scale NeedsState already uses for an
// individual agent - see AgentGroupSimulationSystem.h for the drift
// itself; that simulation logic is unchanged by this rename, only its
// types are. Cohesion/leader-roles/shared-goal fields are still not here -
// that is later roadmap work (2.12D+), once the membership/lifecycle
// layer exists to build on.
//
// Territory is a single point, not a radius/shape/patrol area - whatever
// shape a future materialization policy needs is that policy's own
// decision, not this identity model's. Its map is AgentRecord::MapId - an
// AgentGroup's MapId means "where this group's territory is", the same
// role it already plays for an individual agent's spawn map, not
// duplicated here. Pure value: no Creature*/Map*/Unit* - an AgentGroup
// never binds to a live Creature the way an individual agent does (see
// AgentRecord.h's own comment on SpawnId for this AgentType).
struct AgentGroupState
{
    AgentGroupKind Kind = AgentGroupKind::Loose;

    float TerritoryX = 0.0f;
    float TerritoryY = 0.0f;
    float TerritoryZ = 0.0f;

    // Milestone 2.12B: 0.0-1.0 normalized, like NeedsState's own Hunger/
    // ResourcePressure - Hunger rises over time (a hungrier group), while
    // Resources falls (its territory's available prey/food depleting) -
    // see AgentGroupSimulationSystem::Update() for the exact drift.
    // Nothing replenishes Resources yet (no hunting/consumption feedback
    // loop exists at the group level) - that is later roadmap work, the
    // same way NeedsSystem::SatisfyHunger() is the only thing allowed to
    // lower an individual agent's own Hunger.
    float Hunger = 0.0f;
    float Resources = 0.0f;

    // Milestone 2.12B: a monotonic write counter, the same role
    // AgentEconomyState::Version plays for economy writes - see
    // AgentPersistence::SaveAgentGroupState() for why (async writes are
    // not guaranteed to land in order) and why the bump lives there, not
    // in whatever caller mutates this struct first.
    uint64 Version = 0;
};

#endif // AIWORLD_AGENTGROUPSTATE_H
