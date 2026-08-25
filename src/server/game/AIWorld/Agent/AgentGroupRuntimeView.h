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

#ifndef AIWORLD_AGENTGROUPRUNTIMEVIEW_H
#define AIWORLD_AGENTGROUPRUNTIMEVIEW_H

#include "Define.h"

// Milestone 2.12C: a transient, per-tick snapshot of how many of an
// AgentGroup's persistent members are naturally materialized right now -
// built fresh every coarse tick from a plain AgentRegistry::Find() lookup
// per AgentGroupMembership (each member's own individual-agent
// materialization bookkeeping is already the authority - this never
// touches Map*/Creature* itself, unlike the old CreatureGroupRuntimeView,
// which had to resolve raw creature spawns directly since members were
// not yet independent agents). Never cached on AgentRecord itself.
// LoadedMembers > 0 is the whole invariant AIWorldMgr::
// RunDecisionScheduler() acts on: real members become the authority and
// it stops calling AgentGroupSimulationSystem::Update() for as long as
// it holds. Pure value: no Creature*/Map*/ObjectGuid.
struct AgentGroupRuntimeView
{
    uint32 TotalMembers = 0;
    uint32 LoadedMembers = 0;
};

#endif // AIWORLD_AGENTGROUPRUNTIMEVIEW_H
