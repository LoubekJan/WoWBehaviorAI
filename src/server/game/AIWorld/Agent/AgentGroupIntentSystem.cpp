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

AgentGroupIntent AgentGroupIntentSystem::Evaluate(AgentGroupRecord const& group, AgentGroupCoordinationProfile const& profile,
    std::vector<CoalitionMemberObservation> const& members) const
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

    if (!profile.RegroupEnabled)
        return intent;

    // The single farthest-past-radius check is enough here - unlike
    // CoalitionMaintenanceSystem::Evaluate() (which names ONE specific
    // farthest member for LeaveMember), a Regroup intent is a group-level
    // fact: it does not matter which member triggered it, or how many
    // did - the first Materialized+Alive+same-map member found past
    // RegroupRadius is enough to prove the group as a whole wants to
    // regroup. Per-member decomposition (who specifically still needs to
    // move) is AgentGroupIntentProjector's own job (2.12F2), not this
    // one's.
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

    return intent;
}
