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

#include "SpawnReconciliationPlan.h"
#include "AgentTypeProvenance.h"
#include <unordered_map>
#include <unordered_set>

SpawnReconciliationPlan BuildReconciliationPlan(
    std::vector<CreatureSpawnIdentity> const& census,
    std::vector<AgentSpawnBinding> const& existing)
{
    SpawnReconciliationPlan plan;

    std::unordered_map<uint64, CreatureSpawnIdentity const*> eligibleBySpawnId;
    eligibleBySpawnId.reserve(census.size());
    for (CreatureSpawnIdentity const& identity : census)
        eligibleBySpawnId.emplace(identity.SpawnId, &identity);

    std::unordered_set<uint64> existingSpawnIds;
    existingSpawnIds.reserve(existing.size());
    for (AgentSpawnBinding const& binding : existing)
    {
        existingSpawnIds.insert(binding.SpawnId);

        if (eligibleBySpawnId.find(binding.SpawnId) == eligibleBySpawnId.end())
            plan.Orphaned.push_back(binding.Id);
        else
            ++plan.ValidCount;
    }

    for (CreatureSpawnIdentity const& identity : census)
    {
        if (existingSpawnIds.find(identity.SpawnId) != existingSpawnIds.end())
            continue;

        PendingCreatureAgent pending;
        pending.Type = DeriveCreatureAgentType(identity.NpcFlags);
        pending.MapId = identity.MapId;
        pending.SpawnId = identity.SpawnId;
        plan.Missing.push_back(pending);
    }

    return plan;
}
