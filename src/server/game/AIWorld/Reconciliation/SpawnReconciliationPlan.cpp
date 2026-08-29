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
    std::vector<AgentSpawnBinding> const& physicalBindings)
{
    SpawnReconciliationPlan plan;

    std::unordered_map<uint64, CreatureSpawnIdentity const*> eligibleBySpawnId;
    eligibleBySpawnId.reserve(census.size());
    for (CreatureSpawnIdentity const& identity : census)
        eligibleBySpawnId.emplace(identity.SpawnId, &identity);

    // Every SpawnId with ANY physical ai_agents row - valid, orphaned,
    // conflicted, or already quarantined by LoadAgents() - must be
    // excluded from Missing below. A SpawnId can't collide with the
    // ai_agents UNIQUE (map_id, spawn_id) key if reconciliation never
    // tries to INSERT it again in the first place.
    std::unordered_set<uint64> physicalSpawnIds;
    physicalSpawnIds.reserve(physicalBindings.size());

    for (AgentSpawnBinding const& binding : physicalBindings)
    {
        physicalSpawnIds.insert(binding.SpawnId);

        // 2.12F4A2's own invariant - AgentPersistence::LoadAgents() itself
        // already quarantined this row (never added it to AgentRegistry).
        // Nothing for this plan to remove; just don't treat its SpawnId as
        // Missing.
        if (binding.Id.Value != binding.SpawnId)
        {
            ++plan.QuarantinedCount;
            continue;
        }

        auto it = eligibleBySpawnId.find(binding.SpawnId);
        if (it == eligibleBySpawnId.end())
        {
            plan.Orphaned.push_back(binding.Id);
            continue;
        }

        // Eligible spawn exists, AgentId == SpawnId, but the row's own
        // stored MapId disagrees with what world.creature actually says -
        // a data inconsistency, never silently counted as VALID.
        if (binding.MapId != it->second->MapId)
        {
            plan.Conflicted.push_back(binding.Id);
            continue;
        }

        ++plan.ValidCount;
    }

    for (CreatureSpawnIdentity const& identity : census)
    {
        if (physicalSpawnIds.find(identity.SpawnId) != physicalSpawnIds.end())
            continue;

        PendingCreatureAgent pending;
        pending.Type = DeriveCreatureAgentType(identity.NpcFlags);
        pending.MapId = identity.MapId;
        pending.SpawnId = identity.SpawnId;
        plan.Missing.push_back(pending);
    }

    return plan;
}
