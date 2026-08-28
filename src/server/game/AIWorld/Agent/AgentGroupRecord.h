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

#ifndef AIWORLD_AGENTGROUPRECORD_H
#define AIWORLD_AGENTGROUPRECORD_H

#include "AgentGroupKind.h"
#include "AgentGroupMembership.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"
#include "GroupId.h"
#include <vector>

// Milestone 2.12D: AgentGroupRegistry's own owned state for one AgentGroup -
// replaces AgentGroupState (which lived nested inside an AgentRecord) now
// that a group is no longer a fake AgentRecord at all (see GroupId.h). A
// group is a social layer over independent member agents (Members below,
// each a real AgentId/AgentRecord owned by AgentRegistry), never an
// aggregate that stands in for them - member count is Members.size(), never
// stored as its own field.
//
// No Hunger any more (STATIC review P2 fix): the old AgentGroupState kept a
// group-level Hunger that drifted whenever no member happened to be
// materialized, and paused the moment one was - modeling "the aggregate
// stands in for its members when they're not around", the exact aggregate-
// replaces-members shape this whole rename was meant to remove. Each member
// already has its own NeedsState; a second, group-level Hunger would just be
// a second, competing source of truth for the same fact. Resources stays:
// unlike Hunger it never claimed to represent a member's own need - it is
// the group's shared/environmental territory resource (available prey/food
// in its territory), which legitimately exists independent of whether any
// one member is materialized right now, so its drift no longer pauses for
// member presence either - see AgentGroupSimulationSystem::Update().
//
// TerritoryMapId/TerritoryX/Y/Z: identity only, unchanged by any simulation
// that exists yet - previously borrowed AgentRecord::MapId for the map half,
// which no longer exists here since a group has no MapId/SpawnId of its own.
struct AgentGroupRecord
{
    GroupId Id;
    AgentGroupKind Kind = AgentGroupKind::Loose;

    // Milestone 2.12E4C2 P2 fix (STATIC review): which CoalitionFormationProfile
    // (if any) actually created this group - persistent, and deliberately
    // separate from Kind. Kind alone cannot tell two profiles of the same
    // Kind apart (today only WolfLoose forms Loose groups, but nothing
    // about Kind itself prevents a future second Loose-forming profile),
    // and a manually/admin-created Loose group must never be silently
    // treated as if some automatic profile owned it. Invalid (the default)
    // means exactly that - no automatic formation profile created this
    // group, so no automatic maintenance profile may act on it either; see
    // AIWorldMgr::RunCoalitionMaintenance()'s own candidate filter and
    // CoalitionMaintenanceSystem::Evaluate()'s own fail-closed Invalid/
    // mismatch checks. Set once, at creation
    // (AgentGroupLifecycleSystem::RequestCreateGroup()'s own profileId
    // parameter), and never changed afterward - persisted the same way
    // Kind/Territory* already are (AgentGroupPersistence::CreateGroupAsync()/
    // LoadGroups()/SaveGroupState()).
    CoalitionFormationProfileId ProfileId = CoalitionFormationProfileId::Invalid;

    uint32 TerritoryMapId = 0;
    float TerritoryX = 0.0f;
    float TerritoryY = 0.0f;
    float TerritoryZ = 0.0f;

    // Milestone 2.12B/2.12D: 0.0-1.0 normalized, falling over time - see
    // AgentGroupSimulationSystem::Update() for the exact drift. Nothing
    // replenishes it yet (no hunting/consumption feedback loop exists at
    // the group level) - later roadmap work.
    float Resources = 0.0f;

    // Milestone 2.12B: a monotonic write counter, the same role
    // AgentEconomyState::Version plays for economy writes - see
    // AgentGroupPersistence::SaveGroupState() for why (async writes are not
    // guaranteed to land in order) and why the bump lives there, not in
    // whatever caller mutates this struct first.
    uint64 Version = 0;

    // Milestone 2.12D: persistent membership - which independent member
    // agents (each with its own AgentId/identity/memory/needs/goal/
    // decision/actions/Creature lifecycle) belong to this group, loaded
    // once at startup (AgentGroupPersistence::LoadGroupMembers(), called
    // after both LoadAgents()/AgentGroupPersistence::LoadGroups() since a
    // membership row needs both an already-registered group and an
    // already-registered member to attach to - see AgentGroupMembership.h
    // for why a forward reference is no longer tolerated here, unlike the
    // superseded CreatureGroup model). Never mutated at runtime in this
    // milestone (no create/join/leave/dissolve lifecycle yet).
    std::vector<AgentGroupMembership> Members;
};

#endif // AIWORLD_AGENTGROUPRECORD_H
