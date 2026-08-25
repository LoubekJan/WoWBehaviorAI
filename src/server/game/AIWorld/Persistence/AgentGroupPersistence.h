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
#include "Agent/AgentId.h"
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
//
// Milestone 2.12E1: stateful now, unlike AgentPersistence - owns the
// GroupId allocator (_nextGroupId), the same in-memory "seed from the
// highest id ever seen, then increment per mint" shape
// GuildMgr::GenerateGuildId() already uses for guildid, chosen over a
// MySQL AUTO_INCREMENT read-back (what CreateCreatureAgent() effectively
// does via its unique (map_id, spawn_id) binding) because a group has no
// natural unique binding of its own to read back by - see CreateGroup()'s
// own comment and the 2.12E1 migration's comment on ai_agent_groups.
class TC_GAME_API AgentGroupPersistence
{
    public:
        // Loads every row from ai_agent_groups into registry. Returns the
        // number of groups loaded. Must be called before LoadGroupMembers()
        // (a membership row needs an already-registered group to attach
        // to).
        //
        // Milestone 2.12E1: also (re)seeds the GroupId allocator
        // (_nextGroupId) from the highest group_id this sees - including a
        // row rejected by the AgentGroupKind validation below, since it
        // still physically occupies that id in the table even though it
        // never reaches the registry; seeding from only the successfully-
        // loaded rows could mint a fresh id that collides with a row this
        // load chose to skip rather than trust. Still seeds to 1 (empty
        // table => first mint is id 1) even when zero rows load at all.
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

        // Milestone 2.12E1: mints a fresh GroupId from _nextGroupId (never
        // derived from AgentId/SpawnId/RuntimeGuid - see this class's own
        // comment) and writes it to ai_agent_groups via CONNECTION_SYNCH/
        // DirectExecute(), then reads the row back
        // (CHAR_SEL_AI_AGENT_GROUP_BY_ID) to confirm the write actually
        // landed before returning it - the same "no read-back, no
        // confirmed identity" discipline CreateCreatureAgent() already
        // holds AgentId to, just via the freshly-minted id's own
        // guaranteed-unique value instead of a natural (map_id, spawn_id)
        // binding (a group has neither). Synchronous by design like
        // CreateCreatureAgent() - acceptable here because group creation
        // is a rare, lifecycle-scoped operation (2.12E1 has no automatic
        // group formation yet), never a per-tick one the way
        // SaveGroupState() above is; AddGroupMember()/RemoveGroupMember()/
        // DeleteGroup() below are the same category.
        //
        // _nextGroupId is advanced unconditionally, whether or not the
        // read-back below confirms success - a failed create just burns
        // an id, the same tradeoff GuildMgr::GenerateGuildId() already
        // accepts, rather than risking a retry reusing an id whose insert
        // may actually have landed despite an unrelated read-back hiccup.
        // Returns GroupId{} (Value == 0, never a valid id) if the
        // read-back doesn't find the row - the caller
        // (AgentGroupLifecycleSystem::CreateGroup()) must not add anything
        // to AgentGroupRegistry in that case.
        GroupId CreateGroup(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources);

        // Milestone 2.12E1: CONNECTION_SYNCH/DirectExecute() - see
        // CreateGroup()'s own comment for why synchronous is acceptable
        // for this category of operation. Duplicate-membership prevention
        // is AgentGroupLifecycleSystem::JoinGroup()'s job, checked against
        // the in-memory AgentGroupRecord::Members before this is ever
        // called - this method does not itself detect or reject a
        // duplicate (group_id, member_agent_id) pair.
        void AddGroupMember(GroupId groupId, AgentId memberId, uint64 joinedAtMs);

        // Milestone 2.12E1: CONNECTION_SYNCH/DirectExecute(). A DELETE that
        // matches zero rows (member was never actually in the group) is
        // not an error at this layer - AgentGroupLifecycleSystem::
        // LeaveGroup() is what decides whether that is worth logging,
        // using its own in-memory AgentGroupRecord::Members check, not
        // this method's return.
        void RemoveGroupMember(GroupId groupId, AgentId memberId);

        // Milestone 2.12E1: deletes every ai_agent_group_members row for
        // groupId first (CHAR_DEL_AI_AGENT_GROUP_MEMBERS_BY_GROUP), then
        // the ai_agent_groups row itself (CHAR_DEL_AI_AGENT_GROUP) -
        // deliberately in that order, so an interrupted delete (process
        // crash between the two statements) never leaves an orphaned
        // membership row pointing at a group_id that no longer exists;
        // the reverse order could. Both CONNECTION_SYNCH/DirectExecute(),
        // same category as CreateGroup() above. Never touches ai_agents -
        // AgentGroupLifecycleSystem::DissolveGroup() is the caller's own
        // guarantee that member AgentRecords are never touched by this.
        void DeleteGroup(GroupId groupId);

    private:
        // Milestone 2.12E1: seeded by LoadGroups() at startup, advanced by
        // one per CreateGroup() call thereafter - never reset, never
        // reused once issued (even for a failed create, see CreateGroup()'s
        // own comment). World-thread-only, like everything else here.
        uint64 _nextGroupId = 1;
};

#endif // AIWORLD_AGENTGROUPPERSISTENCE_H
