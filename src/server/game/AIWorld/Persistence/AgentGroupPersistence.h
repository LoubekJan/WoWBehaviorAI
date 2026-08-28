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
#include "Agent/CoalitionFormationProfileId.h"
#include "Agent/GroupId.h"
#include "Define.h"
#include "Transaction.h"
#include <functional>
#include <optional>

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
// own migration comment.
//
// Milestone 2.12E1 P2 fix (STATIC review, rounds 3-4): the allocator is
// fail-closed - _groupIdAllocatorValid tracks whether LoadGroupIdSequence()
// actually found a trustworthy sequence row (nonzero, and strictly greater
// than every group_id physically present in ai_agent_groups) - if it
// didn't, CreateGroupAsync() refuses to mint anything at all.
//
// Milestone 2.12E2 P1 fix (STATIC review): includes Transaction.h (not just
// a forward declaration) because CreateGroupAsync()/AddGroupMemberAsync()/
// RemoveGroupMemberAsync()/DeleteGroupAsync() below return TransactionCallback/
// std::optional<TransactionCallback> BY VALUE - any caller (not just the
// one this codebase happens to have today) needs the complete type to
// call or store the result, not merely to name it.
//
// Milestone 2.12E2: every write below is async - CreateGroupAsync()/
// AddGroupMemberAsync()/RemoveGroupMemberAsync()/DeleteGroupAsync() all
// return a TransactionCallback (never block the calling thread) whose
// AfterComplete() reports a genuine, DB-driver-reported success/failure
// bool - not an inferred "does a read-back match" guess, the actual
// execution status CharacterDatabaseConnection::ExecuteTransaction()
// itself produces. This replaces 2.12E1's synchronous DirectExecute()-
// then-read-back pattern entirely: a CharacterDatabaseTransaction's own
// commit result is a strictly stronger confirmation than any read-back
// could be (a read-back can misread pre-existing, unrelated data as
// confirming a write that never actually happened - see the round-4
// GroupId-sequence fix above for a concrete case of exactly that). The
// caller (AgentGroupLifecycleSystem) supplies an onComplete callback and
// is responsible for enqueuing the returned TransactionCallback into a
// TransactionCallbackProcessor it polls once per world tick - this class
// never touches the processor itself, and never mutates
// AgentGroupRegistry/AgentRecord on its own; it only ever reports the DB
// outcome back to whoever asked.
class TC_GAME_API AgentGroupPersistence
{
    public:
        // Milestone 2.12E1 P2 fix (STATIC review, rounds 3-4): reads the
        // single row from ai_agent_group_id_sequence into _nextGroupId and
        // marks the allocator valid - fail-closed if the row is missing (a
        // schema that never ran the 2.12E1 P2 migration, or a hand-edited
        // table) OR if it is not a trustworthy high-water mark: it must be
        // nonzero and strictly greater than the highest group_id
        // physically present in ai_agent_groups right now (checked against
        // the raw table, not the registry - a row LoadGroups() itself
        // would later reject, e.g. for an invalid kind, still occupies
        // that id and must still be protected against). Any failure logs
        // an error and leaves the allocator invalid, which
        // CreateGroupAsync() checks first and refuses to mint against.
        // Synchronous, startup-only (called once from
        // AIWorldMgr::Initialize(), never from the world update loop) -
        // order relative to LoadGroups()/LoadGroupMembers() does not
        // matter, both checks here query ai_agent_groups directly.
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
        // shape AgentPersistence::SaveEconomyState() already uses. Not
        // part of the 2.12E2 async-lifecycle rework below - this is
        // already fire-and-forget and was never blocking.
        void SaveGroupState(GroupId id, AgentGroupRecord& record);

        // Milestone 2.12E2: mints a fresh GroupId, then submits ONE
        // CharacterDatabaseTransaction containing both the sequence
        // reservation (CHAR_UPD_AI_AGENT_GROUP_ID_SEQUENCE) and the group
        // row INSERT (CHAR_INS_AI_AGENT_GROUP) via
        // CharacterDatabase.AsyncCommitTransaction() - non-blocking, and
        // atomic in a way 2.12E1's synchronous two-step "write, read back,
        // then write, read back" never was: either both land together or
        // neither does, in one round trip. Fail-closed if the allocator is
        // not valid (see LoadGroupIdSequence()) or if _nextGroupId is
        // already the maximum representable uint64 (2.12E2 hardening: an
        // overflow here would wrap the allocator back to 0, a value
        // LoadGroupIdSequence() itself would reject as untrustworthy on
        // the very next restart - refusing outright is simpler and
        // strictly safer than trying to define what "the next id after
        // the last one" even means) - both cases call onComplete(false,
        // GroupId{}) synchronously and return std::nullopt, mint nothing.
        //
        // Otherwise _nextGroupId is advanced in memory IMMEDIATELY,
        // synchronously, before the transaction is even submitted - not
        // after a confirmed write, unlike 2.12E1's synchronous version.
        // This is what makes it safe for two CreateGroupAsync() calls to
        // be in flight at once (both still only ever issued from the
        // world thread, one after another, never concurrently - but the
        // second could easily be requested before the first's transaction
        // has completed): each sees a distinct, already-claimed candidate
        // id, so they can never collide within this process's lifetime.
        //
        // Milestone 2.12E2 P3 fix (STATIC review): a failed transaction is
        // NOT guaranteed to burn its id across a restart - the reservation
        // UPDATE (CHAR_UPD_AI_AGENT_GROUP_ID_SEQUENCE) and the group INSERT
        // are the same transaction, so a failure rolls both back together;
        // the DB's own next_group_id is left exactly where it was before
        // this call, even though _nextGroupId already moved past the
        // failed candidate in this process's memory. If the process
        // restarts before any later CreateGroupAsync() call succeeds and
        // persists a higher value, LoadGroupIdSequence() re-reads the
        // unchanged (lower) sequence and the failed candidate's id can be
        // minted again. This is safe, not a latent collision: a rolled-
        // back transaction never wrote a group row for that id in the
        // first place, so there is nothing for the reused id to collide
        // with - re-minting it is observably identical to never having
        // tried it at all. The only thing this changes from a stricter
        // "every minted id is globally unique across all of history, even
        // failed attempts" invariant is that GroupIds are not guaranteed
        // gapless around a failed create - which nothing in this codebase
        // actually promises or depends on.
        //
        // Returns std::nullopt (onComplete already invoked) if rejected
        // synchronously as above; otherwise returns the TransactionCallback
        // for the caller to enqueue into its own TransactionCallbackProcessor
        // and poll - onComplete(success, newId) fires from there once the
        // transaction's own real commit result is known (newId is only
        // meaningful when success is true).
        //
        // Milestone 2.12E4C2 P2 fix (STATIC review): profileId is bound
        // into the INSERT's own profile_id column - see
        // AgentGroupRecord::ProfileId for what it means and why it is
        // persistent. Always bound explicitly, never left at the column's
        // own DEFAULT - see CHAR_INS_AI_AGENT_GROUP's own comment.
        std::optional<TransactionCallback> CreateGroupAsync(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources,
            CoalitionFormationProfileId profileId, std::function<void(bool success, GroupId newId)> onComplete);

        // Milestone 2.12E2: submits the membership INSERT as its own
        // one-statement CharacterDatabaseTransaction via
        // AsyncCommitTransaction() - non-blocking. Duplicate-membership
        // prevention is still AgentGroupLifecycleSystem::RequestJoinGroup()'s
        // job (checked against the in-memory AgentGroupRecord::Members
        // before this is ever called), not this method's - a duplicate
        // (group_id, member_agent_id) INSERT is not a case this is
        // expected to handle gracefully; the transaction would simply fail
        // and onComplete(false) would fire. Always attempts the write -
        // unlike CreateGroupAsync() this has no synchronous-rejection path
        // of its own, so it always returns a real TransactionCallback for
        // the caller to enqueue.
        TransactionCallback AddGroupMemberAsync(GroupId groupId, AgentId memberId, uint64 joinedAtMs, std::function<void(bool success)> onComplete);

        // Milestone 2.12E2: same shape as AddGroupMemberAsync(), a DELETE
        // instead of an INSERT. A transaction that matches zero rows
        // (member was never actually in the group) still commits
        // successfully - onComplete(true) either way, since the
        // post-condition "not a member in the DB" holds regardless of
        // whether this call's own DELETE was the one that made it true.
        TransactionCallback RemoveGroupMemberAsync(GroupId groupId, AgentId memberId, std::function<void(bool success)> onComplete);

        // Milestone 2.12E2: one CharacterDatabaseTransaction containing
        // both DELETEs (every ai_agent_group_members row for groupId, then
        // the ai_agent_groups row itself) via AsyncCommitTransaction() -
        // atomic and non-blocking, replacing 2.12E1's
        // DirectCommitTransaction()-based synchronous version. Statement
        // order within the transaction still matters for readability (not
        // atomicity - the transaction already guarantees that): membership
        // rows first, group row second. Never touches ai_agents -
        // AgentGroupLifecycleSystem::RequestDissolveGroup() is the
        // caller's own guarantee that member AgentRecords are never
        // touched by this.
        TransactionCallback DeleteGroupAsync(GroupId groupId, std::function<void(bool success)> onComplete);

    private:
        // Milestone 2.12E1/2.12E2: seeded by LoadGroupIdSequence() at
        // startup from the persistent ai_agent_group_id_sequence row,
        // advanced by one - synchronously, immediately, before the
        // matching CreateGroupAsync() transaction is even submitted, see
        // that method's own comment for why - per CreateGroupAsync() call
        // thereafter. Never reset, and never reused once a group actually
        // exists for it, even across a dissolve+restart - that is the
        // whole point of the sequence table (see this class's own
        // comment). Milestone 2.12E2 P3 fix (STATIC review): a FAILED
        // transaction's candidate is a narrower promise than that - see
        // CreateGroupAsync()'s own comment for why it is not guaranteed to
        // stay unminted across a restart, and why that is still safe.
        // World-thread-only, like everything else here. Meaningless unless
        // _groupIdAllocatorValid is true - CreateGroupAsync() checks that
        // first.
        uint64 _nextGroupId = 1;

        // Milestone 2.12E1 P2 fix (STATIC review, round 3): true only once
        // LoadGroupIdSequence() has actually found and validated the
        // ai_agent_group_id_sequence row - fail-closed: CreateGroupAsync()
        // refuses to mint anything while this is false, rather than
        // falling back to _nextGroupId's untrustworthy class-default.
        bool _groupIdAllocatorValid = false;
};

#endif // AIWORLD_AGENTGROUPPERSISTENCE_H
