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

#include "CoalitionMaintenanceSystem.h"
#include "AgentGroupRecord.h"

CoalitionMaintenanceDecision CoalitionMaintenanceSystem::Evaluate(AgentGroupRecord const& group, CoalitionMaintenanceProfile const& profile,
    std::vector<CoalitionMemberObservation> const& members) const
{
    CoalitionMaintenanceDecision decision;
    decision.Group = group.Id;

    // P3 hardening (STATIC review): an Invalid profile (see
    // CoalitionFormationProfileId.h for why Invalid exists and why
    // silently proceeding as if some real profile had been meant is the
    // wrong failure mode) never proposes anything - fail-closed the same
    // way RunCoalitionFormation() already refuses Invalid outright, rather
    // than this method quietly evaluating MinMembers/LeaveRadius values
    // that were never actually configured for any real profile.
    if (profile.ProfileId == CoalitionFormationProfileId::Invalid)
        return decision;

    // P3 hardening (STATIC review): a profile whose own Kind does not
    // match the group actually being evaluated is a caller mismatch (e.g.
    // 2.12E4C2's own orchestration accidentally pairing a WolfLoose
    // (Loose) profile with a Stable group) - never propose anything
    // rather than silently applying one Kind's MinMembers/LeaveRadius
    // rules to a group of a different Kind, which could otherwise leave
    // MEMBERS attached to a mismatched Kind's own configured thresholds.
    if (profile.Kind != group.Kind)
        return decision;

    // 2.12E4C2 P2 fix (STATIC review): a group's own persistent
    // provenance (AgentGroupRecord::ProfileId - which CoalitionFormationProfile,
    // if any, actually created it) must match this profile's own identity
    // too, not just Kind - Kind alone cannot tell two profiles of the same
    // Kind apart, and a manually/admin-created group (ProfileId == Invalid)
    // must never be treated as if some automatic profile owned it just
    // because its Kind happens to match. AIWorldMgr::RunCoalitionMaintenance()
    // already pre-filters its own candidates this way (see its own
    // comment) - this is defense in depth for any other/future caller of
    // Evaluate() that does not.
    if (profile.ProfileId != group.ProfileId)
        return decision;

    // Rule 1: already below minimum, independent of any single member's
    // own distance - see this class's own header comment for why this is
    // not gated on Kind here (AgentGroupPolicySystem::ShouldDissolve() is
    // what actually protects a Stable group from this).
    if (uint32(group.Members.size()) < profile.MinMembers)
    {
        decision.Type = CoalitionMaintenanceDecisionType::DissolveGroup;
        return decision;
    }

    // Rule 2: the single farthest Materialized+Alive+same-map member past
    // LeaveRadius, ties broken by lowest AgentId - see this class's own
    // header comment for why an unloaded/dead/different-map member is
    // never a candidate at all, regardless of whatever position it was
    // last observed at.
    float leaveRadiusSq = profile.LeaveRadius * profile.LeaveRadius;

    CoalitionMemberObservation const* farthest = nullptr;
    float farthestDistanceSq = 0.0f;

    for (CoalitionMemberObservation const& observation : members)
    {
        if (!observation.Materialized || !observation.Alive)
            continue;

        if (observation.MapId != group.TerritoryMapId)
            continue;

        float dx = observation.X - group.TerritoryX;
        float dy = observation.Y - group.TerritoryY;
        float dz = observation.Z - group.TerritoryZ;
        float distanceSq = dx * dx + dy * dy + dz * dz;

        if (distanceSq <= leaveRadiusSq)
            continue;

        bool isNewFarthest = !farthest || distanceSq > farthestDistanceSq ||
            (distanceSq == farthestDistanceSq && observation.MemberId.Value < farthest->MemberId.Value);

        if (isNewFarthest)
        {
            farthest = &observation;
            farthestDistanceSq = distanceSq;
        }
    }

    if (farthest)
    {
        decision.Type = CoalitionMaintenanceDecisionType::LeaveMember;
        decision.Member = farthest->MemberId;
    }

    return decision;
}
