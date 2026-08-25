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
// GroupId allocator (_nextGroupId), the same in-memory "seed once, then
// increment per mint" shape GuildMgr::GenerateGuildId() already uses for
// guildid.
//
// Milestone 2.12E1 P2 fix (STATIC review, round 2): _nextGroupId is seeded
// from ai_agent_group_id_sequence (LoadGroupIdSequence()), NOT from
// MAX(group_id) over the currently-live ai_agent_groups rows any more - a
// dissolved group's row disappears from that MAX(), which would let its id
// be handed to an unrelated group after a restart, directly contradicting
// "never reused once issued" below. The sequence table is append-only
// (never deleted from), so its one row's next_group_id is a true
// high-water mark independent of which groups currently exist - see its
// own migration comment. CreateGroup() persists the reservation there
// before it ever inserts the group row itself.
//
// Every create/join/leave/dissolve write below is followed by (or, for
// DeleteGroup(), wrapped together with) a read-back that confirms the
// write actually landed - DirectExecute() alone never reports success or
// failure, and AgentGroupLifecycleSystem is only allowed to mutate the
// runtime registry once that confirmation holds (STATIC review: a caller
// that mutated the registry right after an unconfirmed DirectExecute()
// could let AgentGroupRegistry silently drift out of agreement with what
// is actually in the DB).
class TC_GAME_API AgentGroupPersistence
{
    public:
        // Milestone 2.12E1 P2 fix (STATIC review): reads the single row
        // from ai_agent_group_id_sequence into _nextGroupId. Call once at
        // startup, before the first CreateGroup() of the process (order
        // relative to LoadGroups()/LoadGroupMembers() does not matter - the
        // sequence table is independent of which groups currently exist).
        // If the row is somehow missing (a schema that never ran the
        // 2.12E1 P2 migration, or a hand-edited table), logs an error and
        // leaves _nextGroupId at its class-default of 1 - not a crash, but
        // a real risk of colliding with an existing group_id, the same
        // "malformed schema state" category AgentPersistence::LoadAgents()
        // already treats as reportable rather than silently tolerated.
        void LoadGroupIdSequence();

        // Loads every row from ai_agent_groups into registry. Returns the
        // number of groups loaded. Must be called before LoadGroupMembers()
        // (a membership row needs an already-registered group to attach
        // to). No longer touches the GroupId allocator (2.12E1 P2 fix) -
        // see LoadGroupIdSequence() for that.
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
        // CreateCreatureAgent() - acceptable for 2.12E1's own startup/
        // admin-scoped usage, but see AgentGroupLifecycleSystem.h for why
        // this must be reconsidered before any automatic/policy-driven
        // caller exists.
        //
        // Milestone 2.12E1 P2 fix (STATIC review, round 2): the reservation
        // (CHAR_UPD_AI_AGENT_GROUP_ID_SEQUENCE) is persisted, and
        // _nextGroupId advanced in memory to match, BEFORE the group row
        // itself is ever inserted - so a process that crashes between the
        // two just burns the reserved id forever (the same accepted
        // tradeoff GuildMgr::GenerateGuildId() already makes), rather than
        // risking two different CreateGroup() calls - across a restart -
        // ever computing the same id. Returns GroupId{} (Value == 0, never
        // a valid id) if the group-row read-back doesn't find the row - the
        // caller (AgentGroupLifecycleSystem::CreateGroup()) must not add
        // anything to AgentGroupRegistry in that case. The reserved id
        // itself is never reused either way, confirmed or not.
        GroupId CreateGroup(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources);

        // Milestone 2.12E1 P2 fix (STATIC review, round 2): CONNECTION_SYNCH/
        // DirectExecute() followed by a CHAR_SEL_AI_AGENT_GROUP_MEMBER
        // read-back confirming the (groupId, memberId) row now exists -
        // returns whether it does. AgentGroupLifecycleSystem::JoinGroup()
        // only adds the membership to the runtime AgentGroupRecord::Members
        // once this returns true - never before. Duplicate-membership
        // prevention is still JoinGroup()'s own job (checked against
        // Members before this is ever called), not this method's; a
        // duplicate (group_id, member_agent_id) INSERT is not a case this
        // method is expected to handle gracefully.
        bool AddGroupMember(GroupId groupId, AgentId memberId, uint64 joinedAtMs);

        // Milestone 2.12E1 P2 fix (STATIC review, round 2): CONNECTION_SYNCH/
        // DirectExecute() followed by the same CHAR_SEL_AI_AGENT_GROUP_MEMBER
        // read-back as AddGroupMember(), here expecting NOT to find the row
        // - returns whether the membership is now confirmed absent (true
        // whether this call actually removed it or it was already gone,
        // since either way the post-condition "not a member in the DB"
        // holds). AgentGroupLifecycleSystem::LeaveGroup() only erases the
        // runtime membership once this returns true.
        bool RemoveGroupMember(GroupId groupId, AgentId memberId);

        // Milestone 2.12E1 P2 fix (STATIC review, round 2): deletes every
        // ai_agent_group_members row for groupId and the ai_agent_groups
        // row itself as ONE CharacterDatabaseTransaction
        // (DirectCommitTransaction() - synchronous, atomic), not two
        // independent DirectExecute() calls - an earlier version issued
        // them separately, which could leave an orphaned membership row if
        // interrupted between the two. Followed by a
        // CHAR_SEL_AI_AGENT_GROUP_BY_ID read-back confirming the group row
        // is now gone; returns that. AgentGroupLifecycleSystem::
        // DissolveGroup() only erases the runtime AgentGroupRecord once
        // this returns true. Never touches ai_agents - DissolveGroup() is
        // the caller's own guarantee that member AgentRecords are never
        // touched by this.
        bool DeleteGroup(GroupId groupId);

    private:
        // Milestone 2.12E1/2.12E1 P2 fix: seeded by LoadGroupIdSequence()
        // at startup from the persistent ai_agent_group_id_sequence row,
        // advanced by one - and immediately re-persisted - per
        // CreateGroup() call thereafter. Never reset, never reused once
        // issued, even across a dissolve+restart (that is the whole point
        // of the sequence table - see this class's own comment).
        // World-thread-only, like everything else here.
        uint64 _nextGroupId = 1;
};

#endif // AIWORLD_AGENTGROUPPERSISTENCE_H
