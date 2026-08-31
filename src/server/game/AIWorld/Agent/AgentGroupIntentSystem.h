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
//   1. If profile.RegroupEnabled, and any member is Materialized, Alive,
//      on the same MapId as group.TerritoryMapId, and further than
//      profile.RegroupRadius from (group.TerritoryX/Y/Z) -> Regroup,
//      targeting the group's own territory point. An unloaded, dead, or
//      different-map member is never a trigger, regardless of its last
//      known position - the same "absence from the grid must never be
//      misread as a social/coordination fact" discipline
//      CoalitionMemberObservation.h already documents for maintenance.
//   2. Otherwise (no Regroup fired - REGROUP always outranks ROAM, a
//      dispersed group must never be pulled further apart by a roam
//      target instead of pulled back together), if profile.RoamEnabled,
//      and any member is Materialized, Alive, on the same MapId as
//      group.TerritoryMapId, and further than profile.RoamArrivalRadius
//      from this call's own deterministic roam target -> Roam, targeting
//      that point. See "Deterministic ROAM target" below for how it is
//      chosen; the same unloaded/dead/different-map exclusions as Regroup
//      apply.
//   3. Otherwise -> None.
// Deliberately targets the group's own fixed TerritoryX/Y/Z (Regroup) or a
// point close to it (Roam), never a dynamically computed centroid of
// currently-materialized members - the simplest choice that proves the
// generic intent/projection pipeline works at all; smarter targeting is a
// later refinement, not required for this milestone's own scope.
//
// Deterministic ROAM target (2.12G2): nowMs (the caller's own current
// time, passed in explicitly rather than read from a clock here - the
// same "every dependency is a parameter" purity every other System class
// in this codebase already holds to) is divided by profile.RoamIntervalMs
// into a phase bucket; group.Id/profile.ProfileId/that phase are mixed
// (RoamPhaseHash(), .cpp) into one of 9 deterministic slots - the
// group's own TerritoryX/Y/Z itself, or one of 8 compass points
// profile.RoamDistance away from it. No rand()/urand()/random_device
// anywhere: the exact same (group, profile, nowMs) always picks the exact
// same slot, so "same GroupId + same roam phase -> same target" holds by
// construction, and the target only actually changes once every
// profile.RoamIntervalMs, not on every coordination pass that happens to
// re-evaluate a still-current phase.
// Fully deterministic given the same group/profile/members/nowMs: two
// calls with the same input always return the same intent.
class TC_GAME_API AgentGroupIntentSystem
{
    public:
        AgentGroupIntent Evaluate(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
            std::vector<CoalitionMemberObservation> const& members, uint64 nowMs) const;
};

#endif // AIWORLD_AGENTGROUPINTENTSYSTEM_H
