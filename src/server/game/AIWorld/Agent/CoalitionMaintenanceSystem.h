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

#ifndef AIWORLD_COALITIONMAINTENANCESYSTEM_H
#define AIWORLD_COALITIONMAINTENANCESYSTEM_H

#include "CoalitionMaintenanceDecision.h"
#include "CoalitionMaintenanceProfile.h"
#include "CoalitionMemberObservation.h"
#include "Define.h"
#include <vector>

struct AgentGroupRecord;

// Milestone 2.12E4C1: "does this existing AgentGroup's current membership
// still make spatial/numeric sense?" - a pure value transform, the same
// shape AgentGroupPolicySystem/CoalitionFormationSystem already are: no DB,
// no AgentRegistry/AgentGroupRegistry mutation, no Creature*/Map*/Player*,
// no async code, nothing held as member state. Every dependency (the
// group, the profile, each member's own observation) is a parameter; every
// call is independently reproducible from its inputs alone. Never mutates
// group or members, never calls AgentGroupLifecycleSystem/
// AgentGroupPolicySystem itself - turning a decision into an actual
// RequestLeaveGroupWithPolicy()/RequestDissolveGroupWithPolicy() call
// (both already policy-gated) is AIWorldMgr's own job (2.12E4C2).
//
// Deliberately does NOT special-case AgentGroupKind::Stable anywhere -
// Evaluate() may return LeaveMember/DissolveGroup for a Stable group
// exactly as readily as for a Loose one. Stable protection already has
// exactly one home, AgentGroupPolicySystem::CanLeave()/ShouldDissolve()
// (StableGroupProtected / "never automatically dissolved" - see
// AgentGroupPolicySystem.h), and 2.12E4C2's own AutomaticPolicy
// Request*WithPolicy() calls already run every proposal this class makes
// through that same gate before anything is ever mutated. Duplicating a
// Stable check here would just be a second, independently maintained copy
// of a rule AgentGroupPolicySystem already owns - exactly the kind of
// drift CoalitionFormationProfile.h's own MinMembers/MaxMembers comment
// already warns against for a different pair of rules.
//
// Rules, evaluated in this order:
//   0a. profile.ProfileId == Invalid -> None (2.12E4C1 P3 hardening,
//       STATIC review: fail-closed, the same reason
//       RunCoalitionFormation() already refuses Invalid outright - see
//       CoalitionFormationProfileId.h). Never evaluates MinMembers/
//       LeaveRadius values that were never actually configured for any
//       real profile.
//   0b. profile.Kind != group.Kind -> None (2.12E4C1 P3 hardening, STATIC
//       review: a caller mismatch - e.g. pairing a Loose profile with a
//       Stable group - never silently applies one Kind's own thresholds
//       to a group of a different Kind).
//   0c. profile.ProfileId != group.ProfileId -> None (2.12E4C2 P2 fix,
//       STATIC review: Kind alone cannot tell two profiles of the same
//       Kind apart, and a manually/admin-created group - group.ProfileId
//       == Invalid - must never be swept into some automatic profile's
//       own maintenance just because its Kind happens to match. See
//       AgentGroupRecord::ProfileId for the persistent provenance this
//       checks).
//   1. group.Members.size() < profile.MinMembers (right now, independent
//      of any single member's own distance) -> DissolveGroup.
//   2. Otherwise, among members, the single FARTHEST one that is
//      Materialized, Alive, on the same MapId as group.TerritoryMapId, and
//      further than profile.LeaveRadius from (group.TerritoryX/Y/Z) ->
//      LeaveMember for that one (ties broken by lowest AgentId, for
//      determinism). A member that is not Materialized, not Alive, on a
//      different MapId, or simply absent from members entirely is never a
//      Leave candidate - see CoalitionMemberObservation.h for why "the
//      grid does not currently have this member loaded" must never be
//      misread as "this member left the group": an unload/death is a fact
//      about Creature/world-state, not about the social membership edge
//      AgentGroupRegistry owns.
//   3. Otherwise -> None.
// Fully deterministic given the same group/profile/members: two calls with
// the same input always return the same decision.
class TC_GAME_API CoalitionMaintenanceSystem
{
    public:
        CoalitionMaintenanceDecision Evaluate(AgentGroupRecord const& group, CoalitionMaintenanceProfile const& profile,
            std::vector<CoalitionMemberObservation> const& members) const;
};

#endif // AIWORLD_COALITIONMAINTENANCESYSTEM_H
