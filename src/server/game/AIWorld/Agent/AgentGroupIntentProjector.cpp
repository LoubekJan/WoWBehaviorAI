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

#include "AgentGroupIntentProjector.h"

std::vector<GroupMemberActionProposal> AgentGroupIntentProjector::Project(AgentGroupIntent const& intent, AgentGroupCoordinationProfile const& profile,
    std::vector<CoalitionMemberObservation> const& members) const
{
    std::vector<GroupMemberActionProposal> proposals;

    if (intent.Type == AgentGroupIntentType::None)
        return proposals;

    float regroupRadiusSq = profile.RegroupRadius * profile.RegroupRadius;

    for (CoalitionMemberObservation const& observation : members)
    {
        if (!observation.Materialized || !observation.Alive)
            continue;

        if (observation.MapId != intent.MapId)
            continue;

        float dx = observation.X - intent.X;
        float dy = observation.Y - intent.Y;
        float dz = observation.Z - intent.Z;
        float distanceSq = dx * dx + dy * dy + dz * dz;

        if (distanceSq <= regroupRadiusSq)
            continue;

        GroupMemberActionProposal proposal;
        proposal.SourceGroup = intent.Group;
        proposal.Member = observation.MemberId;
        proposal.SourceIntent = intent.Type;
        proposal.MapId = intent.MapId;
        proposal.X = intent.X;
        proposal.Y = intent.Y;
        proposal.Z = intent.Z;
        proposals.push_back(proposal);
    }

    return proposals;
}
