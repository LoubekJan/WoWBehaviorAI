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

#ifndef AIWORLD_SPAWNRECONCILIATIONPLAN_H
#define AIWORLD_SPAWNRECONCILIATIONPLAN_H

#include "Agent/AgentId.h"
#include "Agent/AgentSpawnBinding.h"
#include "Agent/PendingCreatureAgent.h"
#include "CreatureSpawnIdentity.h"
#include "Define.h"
#include <vector>

// Milestone 2.12F4B: the three-way diff between the eligible world.creature
// census and what AgentRegistry already knows about, as a pure value - see
// BuildReconciliationPlan()'s own comment. VALID (spawn exists AND an
// ai_agents row/AgentRecord already exists for it) needs no action and is
// therefore not materialized here, only counted.
struct SpawnReconciliationPlan
{
    // Census entries with no corresponding existing binding - reconcile by
    // creating a new AgentRecord (AgentId = SpawnId, ControlMode =
    // ObserveOnly - reconciliation never mass-grants AIWorldControlled).
    std::vector<PendingCreatureAgent> Missing;

    // Existing bindings whose SpawnId is no longer in the eligible census
    // (world.creature spawn removed, or no longer eligible) - reconcile by
    // removing from AgentRegistry only (fail-closed quarantine, never an
    // aggressive ai_agents DELETE - see AIWorldMgr::
    // RunSpawnReconciliation()'s own comment).
    std::vector<AgentId> Orphaned;

    uint32 ValidCount = 0;
};

// Pure - no DB/live pointers. `existing` is every currently-registered
// agent's own (AgentId, MapId, SpawnId) binding, built by the caller from
// AgentRegistry (already in memory - this never re-queries the DB just to
// learn what AgentPersistence::LoadAgents() already loaded moments
// earlier). Every entry in `existing` is assumed to already satisfy
// AgentId == SpawnId - LoadAgents() itself already fails closed
// (quarantines, never adds to the registry) on a row that doesn't, per
// 2.12F4A2, so this never has to re-check that here.
//
// Matching is done purely on SpawnId, not the (MapId, SpawnId) pair:
// TrinityCore's own `creature.guid` (this function's CreatureSpawnIdentity
// ::SpawnId, and AgentSpawnBinding::SpawnId/AgentId::Value after
// 2.12F4A2) is a single global AUTO_INCREMENT primary key across the WHOLE
// `creature` table, not scoped per map (see ObjectMgr::LoadCreatures()'s
// own single flat _creatureDataStore, keyed purely by guid) - the same
// global-uniqueness assumption 2.12F4A2's own AgentId == SpawnId invariant
// already depends on.
TC_GAME_API SpawnReconciliationPlan BuildReconciliationPlan(
    std::vector<CreatureSpawnIdentity> const& census,
    std::vector<AgentSpawnBinding> const& existing);

#endif // AIWORLD_SPAWNRECONCILIATIONPLAN_H
