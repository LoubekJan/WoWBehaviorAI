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

// Milestone 2.12F4B: the diff between the eligible world.creature census
// and what ai_agents PHYSICALLY holds, as a pure value - see
// BuildReconciliationPlan()'s own comment for why this must be the raw
// table, not just what AgentRegistry ended up with after AgentPersistence::
// LoadAgents()'s own fail-closed loading rules. VALID (spawn exists AND a
// correct ai_agents row already exists for it) needs no action and is
// therefore not materialized here, only counted.
struct SpawnReconciliationPlan
{
    // Census entries with NO physical ai_agents row at all for their
    // SpawnId - reconcile by creating a new AgentRecord (AgentId =
    // SpawnId, ControlMode = ObserveOnly - reconciliation never
    // mass-grants AIWorldControlled). A SpawnId that already has a
    // physical row - even a quarantined/conflicted one - is never placed
    // here (2.12F4B P2 fix, STATIC review): attempting to INSERT it again
    // would collide with ai_agents' own UNIQUE (map_id, spawn_id) key.
    std::vector<PendingCreatureAgent> Missing;

    // Existing, AgentId==SpawnId-valid bindings whose SpawnId is no longer
    // in the eligible census (world.creature spawn removed, or no longer
    // eligible) - reconcile by removing from AgentRegistry only
    // (fail-closed quarantine, never an aggressive ai_agents DELETE - see
    // AIWorldMgr::RunSpawnReconciliation()'s own comment).
    std::vector<AgentId> Orphaned;

    // Existing, AgentId==SpawnId-valid bindings for a still-eligible
    // SpawnId, but whose own stored MapId does NOT match what
    // world.creature actually says (2.12F4B P2 fix, STATIC review: this
    // was previously silently counted as VALID, matching on SpawnId alone
    // and ignoring MapId entirely). A data inconsistency, not a legitimate
    // state - reconciled the same way as Orphaned: removed from
    // AgentRegistry only, never auto-repaired/DELETEd, since deciding how
    // to fix a wrong MapId is not this milestone's job.
    std::vector<AgentId> Conflicted;

    uint32 ValidCount = 0;

    // Physical rows LoadAgents() itself already quarantined (agent_id !=
    // spawn_id, 2.12F4A2) - never in AgentRegistry to begin with, so there
    // is nothing for this plan to remove for them. Counted here only so
    // RunSpawnReconciliation()'s own summary log doesn't have to silently
    // drop them; they still correctly keep their SpawnId out of Missing.
    uint32 QuarantinedCount = 0;
};

// Pure - no DB/live pointers. `physicalBindings` is the CURRENT, COMPLETE
// content of ai_agents (AgentPersistence::LoadAllBindings() - every row,
// including ones AgentPersistence::LoadAgents() itself refused to add to
// AgentRegistry). This is deliberately NOT built from AgentRegistry
// (2.12F4B P2 fix, STATIC review): AgentRegistry only reflects rows that
// already passed LoadAgents()'s own AgentId == SpawnId check, so a
// quarantined row's (map_id, spawn_id) binding would otherwise be
// invisible to this diff and get classified as Missing - and the
// resulting INSERT would then collide with that quarantined row's own
// UNIQUE (map_id, spawn_id) key.
//
// Matching against the census is done primarily on SpawnId, not the
// (MapId, SpawnId) pair: TrinityCore's own `creature.guid` (this
// function's CreatureSpawnIdentity::SpawnId, and AgentSpawnBinding::
// SpawnId/AgentId::Value after 2.12F4A2) is a single global
// AUTO_INCREMENT primary key across the WHOLE `creature` table, not
// scoped per map (see ObjectMgr::LoadCreatures()'s own single flat
// _creatureDataStore, keyed purely by guid) - the same global-uniqueness
// assumption 2.12F4A2's own AgentId == SpawnId invariant already depends
// on. MapId is still cross-checked once a SpawnId match is found (see
// Conflicted above) - SpawnId uniqueness does not excuse a stored row
// disagreeing with world.creature about which map that spawn is on.
TC_GAME_API SpawnReconciliationPlan BuildReconciliationPlan(
    std::vector<CreatureSpawnIdentity> const& census,
    std::vector<AgentSpawnBinding> const& physicalBindings);

#endif // AIWORLD_SPAWNRECONCILIATIONPLAN_H
