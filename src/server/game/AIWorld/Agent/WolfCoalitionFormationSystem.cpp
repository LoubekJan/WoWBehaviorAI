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

#include "WolfCoalitionFormationSystem.h"
#include <algorithm>

std::optional<WolfCoalitionProposal> WolfCoalitionFormationSystem::Propose(std::vector<WolfCoalitionCandidate> const& candidates, WolfCoalitionFormationConfig const& config) const
{
    if (uint32(candidates.size()) < config.MinMembers)
        return std::nullopt;

    // Deterministic seed selection: lowest AgentId, regardless of the
    // caller's own iteration order - see this class's own header comment.
    std::vector<WolfCoalitionCandidate> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(),
        [](WolfCoalitionCandidate const& a, WolfCoalitionCandidate const& b) { return a.Id.Value < b.Id.Value; });

    WolfCoalitionCandidate const& seed = sorted.front();
    float radiusSq = config.RadiusYards * config.RadiusYards;

    std::vector<WolfCoalitionCandidate> neighbors;
    for (std::size_t i = 1; i < sorted.size(); ++i)
    {
        WolfCoalitionCandidate const& candidate = sorted[i];
        if (candidate.MapId != seed.MapId)
            continue;

        float dx = candidate.X - seed.X;
        float dy = candidate.Y - seed.Y;
        float dz = candidate.Z - seed.Z;
        float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq > radiusSq)
            continue;

        neighbors.push_back(candidate);
    }

    std::sort(neighbors.begin(), neighbors.end(),
        [&seed](WolfCoalitionCandidate const& a, WolfCoalitionCandidate const& b)
        {
            float dxa = a.X - seed.X, dya = a.Y - seed.Y, dza = a.Z - seed.Z;
            float dxb = b.X - seed.X, dyb = b.Y - seed.Y, dzb = b.Z - seed.Z;
            float distanceSqA = dxa * dxa + dya * dya + dza * dza;
            float distanceSqB = dxb * dxb + dyb * dyb + dzb * dzb;
            if (distanceSqA != distanceSqB)
                return distanceSqA < distanceSqB;
            return a.Id.Value < b.Id.Value;
        });

    // config.MaxMembers is always >= 1 by the time it reaches here - copied
    // from AgentGroupPolicyConfig::LooseMaxMembers, which AIWorldMgr::
    // Initialize() already clamps to at least AIWorld.LooseGroupMinMembers
    // (itself clamped to at least 1) - so MaxMembers - 1 never underflows.
    if (uint32(neighbors.size()) > config.MaxMembers - 1)
        neighbors.resize(config.MaxMembers - 1);

    if (uint32(1 + neighbors.size()) < config.MinMembers)
        return std::nullopt;

    WolfCoalitionProposal proposal;
    proposal.MapId = seed.MapId;
    proposal.TerritoryX = seed.X;
    proposal.TerritoryY = seed.Y;
    proposal.TerritoryZ = seed.Z;
    proposal.Members.push_back(seed.Id);
    for (WolfCoalitionCandidate const& neighbor : neighbors)
        proposal.Members.push_back(neighbor.Id);

    return proposal;
}
