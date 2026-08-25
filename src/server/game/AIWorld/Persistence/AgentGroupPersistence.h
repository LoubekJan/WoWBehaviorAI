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

#ifndef AIWORLD_AGENTGROUPPERSISTENCE_H
#define AIWORLD_AGENTGROUPPERSISTENCE_H

#include "Agent/AgentGroupRecord.h"
#include "Agent/GroupId.h"
#include "Define.h"

class AgentGroupRegistry;
class AgentRegistry;

// Milestone 2.12D (STATIC review P2 fix): characters-DB-backed persistence
// for AgentGroup identity (ai_agent_groups) and membership
// (ai_agent_group_members) - the group-side counterpart to AgentPersistence,
// deliberately its own class now that a group is no longer an ai_agents row
// at all (see GroupId.h/AgentGroupRecord.h/AgentGroupRegistry.h). Same
// synchronous-at-startup/async-from-the-update-loop split AgentPersistence
// already uses, for the same reasons - see its own class comment.
class TC_GAME_API AgentGroupPersistence
{
    public:
        // Loads every row from ai_agent_groups into registry. Returns the
        // number of groups loaded. Must be called before LoadGroupMembers()
        // (a membership row needs an already-registered group to attach
        // to).
        uint32 LoadGroups(AgentGroupRegistry& registry);

        // Loads every row from ai_agent_group_members into the matching
        // AgentGroupRecord::Members - must be called after both
        // LoadGroups() and AgentPersistence::LoadAgents() (a membership row
        // needs both an already-registered group and an already-registered
        // member to attach to). A row whose group_id does not resolve to a
        // registered group is an orphan (no FK enforces this at the DB
        // level, the same tolerance ai_long_term_memories already has) -
        // logged and skipped, never fatal.
        //
        // Milestone 2.12D P2 fix (STATIC review): unlike the superseded
        // CreatureGroup model, member_agent_id is NOT tolerated as a
        // forward reference any more - it must already resolve to a
        // registered AgentRecord in agentRegistry, or the row is logged
        // and skipped the same way an orphaned group_id is. agentRegistry
        // is read-only here; this never mutates it. Returns the number of
        // memberships loaded.
        uint32 LoadGroupMembers(AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry);

        // Milestone 2.12B/2.12D: fire-and-forget async UPDATE
        // (CONNECTION_ASYNC/Execute(), never CONNECTION_SYNCH/
        // DirectExecute()) - meant to be called from the world update loop
        // (right after AgentGroupSimulationSystem::Update() mutates an
        // AgentGroupRecord in memory), and record is a mutable reference
        // for the same reason SaveEconomyState()'s own does: this function
        // increments record.Version itself, unconditionally, as its first
        // step, before persisting - never trusted to the caller. Writes
        // the whole group row (Kind/Territory* included, not just
        // Resources) every time, the same "persist the whole snapshot"
        // shape AgentPersistence::SaveEconomyState() already uses.
        void SaveGroupState(GroupId id, AgentGroupRecord& record);
};

#endif // AIWORLD_AGENTGROUPPERSISTENCE_H
