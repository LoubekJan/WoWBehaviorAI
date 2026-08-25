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

// Milestone 2.12C: a member AgentId edge - replaces the old
// CreatureGroupMember (a raw (MapId, SpawnId) creature spawn reference).
// A member is now an independent agent with its own identity/memory/
// needs/goal/decision/actions/Creature lifecycle, not a raw spawn the
// group looked up directly - so membership is expressed the same way any
// other agent relationship in this codebase is, by AgentId. Loaded once
// at startup (AgentPersistence::LoadAgentGroupMembers(), called after
// LoadAgents() since a membership row needs an already-registered
// group_agent_id to attach to) from ai_agent_group_members and never
// mutated at runtime in this milestone - no create/join/leave/dissolve
// lifecycle yet (that is 2.12D's job). Member does not have to already
// resolve to a registered agent - a membership row naming an agent that
// does not exist yet is a legitimate forward reference (e.g. seeded
// before the individual wolf agents 2.12E eventually creates), not an
// error; AIWorldMgr::RunDecisionScheduler() already tolerates a
// currently-unresolvable member when building an AgentGroupRuntimeView.
// Pure value: no Creature*/Map*/ObjectGuid.
struct AgentGroupMembership
{
    AgentId Member;
    uint64 JoinedAtMs = 0;
};

#endif // AIWORLD_AGENTGROUPMEMBERSHIP_H
