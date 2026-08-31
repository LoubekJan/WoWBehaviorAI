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

#include "HuntIntentProjector.h"

std::vector<HuntProposal> HuntIntentProjector::Project(HuntIntent const& intent, std::vector<CoalitionMemberObservation> const& members) const
{
    std::vector<HuntProposal> proposals;

    // See this class's own header comment for why this is a narrow
    // "is this even a real intent" check, not a re-derivation of
    // HuntIntentSystem's own target-eligibility rules.
    if (!intent.Group)
        return proposals;

    if (intent.Target.TargetGuid.IsEmpty())
        return proposals;

    if (intent.Target.TargetEntry == 0)
        return proposals;

    if (!intent.Target.Alive)
        return proposals;

    if (intent.Target.ObservedAtMs == 0)
        return proposals;

    if (intent.StartedAtMs == 0)
        return proposals;

    for (CoalitionMemberObservation const& observation : members)
    {
        if (!observation.Materialized || !observation.Alive)
            continue;

        if (observation.MapId != intent.Target.MapId)
            continue;

        HuntProposal proposal;
        proposal.SourceGroup = intent.Group;
        proposal.Member = observation.MemberId;
        proposal.Target = intent.Target;
        proposal.StartedAtMs = intent.StartedAtMs;
        proposals.push_back(proposal);
    }

    return proposals;
}
