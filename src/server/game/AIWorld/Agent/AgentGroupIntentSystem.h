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

#ifndef AIWORLD_AGENTGROUPINTENTSYSTEM_H
#define AIWORLD_AGENTGROUPINTENTSYSTEM_H

#include "AgentGroupCoordinationProfile.h"
#include "AgentGroupIntent.h"
#include "CoalitionMemberObservation.h"
#include "Define.h"
#include <vector>

struct AgentGroupRecord;

// Milestone 2.12F1: "does this group currently want anything, as a
// group?" - a pure value transform, the same shape AgentGroupPolicySystem/
// CoalitionFormationSystem/CoalitionMaintenanceSystem already are: no DB,
// no AgentRegistry/AgentGroupRegistry mutation, no Creature*/Map*/
// Player*, no async code, nothing held as member state, and NO knowledge
// of any specific profile's own identity (WolfLoose, or any future
// GuardPatrol/BanditGroup/Caravan/CivilianGroup) - this class never
// branches on CoalitionFormationProfileId anywhere in its own logic, only
// on the generic AgentGroupCoordinationProfile fields a caller already
// resolved for whichever profile actually applies. Turning an intent into
// individual member action proposals (AgentGroupIntentProjector, 2.12F2)
// and from there into real ActionRequests (ActionSystem, already the sole
// authority over anything that reaches TrinityCore) is deliberately NOT
// this class's job - see AgentGroupIntent.h for that boundary. A group
// never gets its own equivalent of group.MoveTo(...); only an individual
// Agent's own eventual ActionRequest ever moves anything.
//
// Rules, evaluated in this order (the same fail-closed shape
// CoalitionMaintenanceSystem::Evaluate() already holds its own profile
// checks to):
//   0a. profile.ProfileId == Invalid -> None.
//   0b. profile.Kind != group.Kind -> None.
//   0c. profile.ProfileId != group.ProfileId -> None (see
//       AgentGroupRecord::ProfileId for why Kind alone is never enough to
//       tell two profiles of the same Kind apart).
//   0d. !profile.RegroupEnabled -> None - a profile that has not opted
//       into automatic regrouping never gets one proposed, regardless of
//       how dispersed its members are.
//   1. Otherwise, if any member is Materialized, Alive, on the same MapId
//      as group.TerritoryMapId, and further than profile.RegroupRadius
//      from (group.TerritoryX/Y/Z) -> Regroup, targeting the group's own
//      territory point. An unloaded, dead, or different-map member is
//      never a trigger, regardless of its last known position - the same
//      "absence from the grid must never be misread as a social/
//      coordination fact" discipline CoalitionMemberObservation.h already
//      documents for maintenance.
//   2. Otherwise -> None.
// Deliberately targets the group's own fixed TerritoryX/Y/Z, not a
// dynamically computed centroid of currently-materialized members - the
// simplest choice that proves the generic intent/projection pipeline
// works at all; a centroid-based (or otherwise smarter) regroup target is
// a later refinement, not required for this milestone's own scope.
// Fully deterministic given the same group/profile/members: two calls
// with the same input always return the same intent.
class TC_GAME_API AgentGroupIntentSystem
{
    public:
        AgentGroupIntent Evaluate(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
            std::vector<CoalitionMemberObservation> const& members) const;
};

#endif // AIWORLD_AGENTGROUPINTENTSYSTEM_H
