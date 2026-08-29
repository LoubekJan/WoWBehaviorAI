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

#ifndef AIWORLD_AGENTSPAWNBINDING_H
#define AIWORLD_AGENTSPAWNBINDING_H

#include "AgentId.h"
#include "Define.h"

// Milestone 2.12F4B: the (AgentId, MapId, SpawnId) identity of one
// existing ai_agents row - deliberately narrower than AgentRecord/
// AgentPersistence::LoadAgents()'s own full row (no economy/control_mode/
// home/work). Used where only identity/binding matters: AgentPersistence::
// LoadAllBindings() (bulk post-batch-insert confirmation) and
// SpawnReconciliationPlan's own "what already exists" diff input.
struct AgentSpawnBinding
{
    AgentId Id;
    uint32 MapId = 0;
    uint64 SpawnId = 0;
};

#endif // AIWORLD_AGENTSPAWNBINDING_H
