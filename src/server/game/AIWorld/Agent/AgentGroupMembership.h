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

#ifndef AIWORLD_AGENTGROUPMEMBERSHIP_H
#define AIWORLD_AGENTGROUPMEMBERSHIP_H

#include "AgentId.h"
#include "Define.h"

// Milestone 2.12C/2.12D: a member AgentId edge - replaces the old
// CreatureGroupMember (a raw (MapId, SpawnId) creature spawn reference).
// A member is an independent agent with its own identity/memory/needs/
// goal/decision/actions/Creature lifecycle, not a raw spawn the group
// looked up directly - so membership is expressed the same way any other
// agent relationship in this codebase is, by AgentId. Loaded once at
// startup (AgentGroupPersistence::LoadGroupMembers(), called after both
// AgentPersistence::LoadAgents() and AgentGroupPersistence::LoadGroups(),
// since a membership row needs both an already-registered member and an
// already-registered group to attach to) from ai_agent_group_members and
// never mutated at runtime in this milestone - no create/join/leave/
// dissolve lifecycle yet (later roadmap work).
//
// Milestone 2.12D P2 fix (STATIC review): Member MUST already resolve to a
// registered AgentRecord at load time - LoadGroupMembers() rejects (logs,
// skips) any row whose member_agent_id does not. The old "forward
// reference is legitimate" tolerance modeled a real gap in the superseded
// CreatureGroup design, where a group could exist before any of its
// members did; now that group and member identity are fully separate
// (GroupId vs AgentId - see GroupId.h), there is no reason to allow a
// membership edge naming an agent that was never actually created. The
// correct order is: create/load the individual agent first (AgentRegistry
// owns its AgentId), then create the membership edge that names it.
// Pure value: no Creature*/Map*/ObjectGuid.
struct AgentGroupMembership
{
    AgentId Member;
    uint64 JoinedAtMs = 0;
};

#endif // AIWORLD_AGENTGROUPMEMBERSHIP_H
