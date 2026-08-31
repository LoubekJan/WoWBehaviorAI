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

#include "AgentGroupIntentSystem.h"
#include "AgentGroupRecord.h"
#include <cmath>

namespace
{
    // Milestone 2.12G2: deterministic 9-slot ROAM target selection (the
    // group's own territory anchor, or one of 8 compass points around
    // it) - no rand()/urand()/random_device anywhere. This is a plain
    // avalanche mix (SplitMix64's own well-characterized finalizer), not
    // a cryptographic hash - it only needs to spread three small,
    // structured integers across the output range well enough that
    // adjacent GroupIds/phases do not visibly cluster on the same slot,
    // never anything security-sensitive. Same inputs always produce the
    // same output, which is the only property AgentGroupIntentSystem
    // actually depends on for its own "same GroupId + same roam phase ->
    // same target" guarantee.
    uint64 RoamPhaseHash(uint64 groupId, CoalitionFormationProfileId profileId, uint64 phase)
    {
        uint64 seed = groupId ^ (uint64(profileId) * 0x9E3779B97F4A7C15ULL) ^ (phase * 0xBF58476D1CE4E5B9ULL);
        seed ^= seed >> 30;
        seed *= 0xBF58476D1CE4E5B9ULL;
        seed ^= seed >> 27;
        seed *= 0x94D049BB133111EBULL;
        seed ^= seed >> 31;
        return seed;
    }
}

AgentGroupIntent AgentGroupIntentSystem::Evaluate(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
    std::vector<CoalitionMemberObservation> const& members, uint64 nowMs) const
{
    AgentGroupIntent intent;
    intent.Group = group.Id;

    // Fail-closed profile checks - see this class's own header comment
    // for why each of these is checked independently rather than trusting
    // that a non-Invalid profileId already implies a matching Kind, or
    // that a matching Kind already implies a matching ProfileId.
    if (profile.ProfileId == CoalitionFormationProfileId::Invalid)
        return intent;

    if (profile.Kind != group.Kind)
        return intent;

    if (profile.ProfileId != group.ProfileId)
        return intent;

    // Milestone 2.12G2: Regroup is checked (and, on a hit, returned)
    // COMPLETELY before Roam is even considered - REGROUP must always
    // outrank ROAM, a dispersed group must never be pulled even further
    // apart by an automatic roam target while it is still trying to pull
    // itself back together. See this class's own header comment for the
    // full rule ordering.
    if (profile.RegroupEnabled)
    {
        // The single farthest-past-radius check is enough here - unlike
        // CoalitionMaintenanceSystem::Evaluate() (which names ONE
        // specific farthest member for LeaveMember), a Regroup intent is
        // a group-level fact: it does not matter which member triggered
        // it, or how many did - the first Materialized+Alive+same-map
        // member found past RegroupRadius is enough to prove the group as
        // a whole wants to regroup. Per-member decomposition (who
        // specifically still needs to move) is AgentGroupIntentProjector's
        // own job (2.12F2), not this one's.
        float regroupRadiusSq = profile.RegroupRadius * profile.RegroupRadius;

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

            if (distanceSq <= regroupRadiusSq)
                continue;

            intent.Type = AgentGroupIntentType::Regroup;
            intent.MapId = group.TerritoryMapId;
            intent.X = group.TerritoryX;
            intent.Y = group.TerritoryY;
            intent.Z = group.TerritoryZ;
            return intent;
        }
    }

    if (profile.RoamEnabled && profile.RoamIntervalMs > 0)
    {
        // See this class's own header comment ("Deterministic ROAM
        // target") for why this is a pure function of (group, profile,
        // nowMs) rather than any stored/mutable per-group state.
        uint64 phase = nowMs / profile.RoamIntervalMs;
        uint64 slot = RoamPhaseHash(group.Id.Value, profile.ProfileId, phase) % 9;

        float targetX = group.TerritoryX;
        float targetY = group.TerritoryY;
        float const targetZ = group.TerritoryZ;

        if (slot != 0)
        {
            constexpr float TwoPi = 6.283185307179586f;
            float angle = float(slot - 1) * (TwoPi / 8.0f);
            targetX += profile.RoamDistance * std::cos(angle);
            targetY += profile.RoamDistance * std::sin(angle);
        }

        float roamArrivalRadiusSq = profile.RoamArrivalRadius * profile.RoamArrivalRadius;

        for (CoalitionMemberObservation const& observation : members)
        {
            if (!observation.Materialized || !observation.Alive)
                continue;

            if (observation.MapId != group.TerritoryMapId)
                continue;

            float dx = observation.X - targetX;
            float dy = observation.Y - targetY;
            float dz = observation.Z - targetZ;
            float distanceSq = dx * dx + dy * dy + dz * dz;

            if (distanceSq <= roamArrivalRadiusSq)
                continue;

            intent.Type = AgentGroupIntentType::Roam;
            intent.MapId = group.TerritoryMapId;
            intent.X = targetX;
            intent.Y = targetY;
            intent.Z = targetZ;
            return intent;
        }
    }

    return intent;
}
