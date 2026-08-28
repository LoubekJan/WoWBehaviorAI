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

#ifndef AIWORLD_AGENTGROUPLIFECYCLESYSTEM_H
#define AIWORLD_AGENTGROUPLIFECYCLESYSTEM_H

#include "AgentGroupKind.h"
#include "AgentId.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"
#include "GroupId.h"
#include "Persistence/TransactionCallbackProcessor.h"
#include <functional>
#include <optional>
#include <unordered_set>

class AgentGroupPersistence;
class AgentGroupRegistry;
class AgentRegistry;

// Milestone 2.12E1: the single owner of AgentGroup create/join/leave/
// dissolve - no caller (AIWorldMgr, and later any admin/test command) is
// meant to mutate AgentGroupRegistry membership or call
// AgentGroupPersistence's create/delete surface directly; everything goes
// through here instead. Unlike AgentGroupSimulationSystem (a pure value
// transform with no DB/registry access of its own), this class deliberately
// does own that orchestration - it is the one place that enforces "DB
// write happens, and is confirmed, before the runtime registry is ever
// mutated to match" (see RequestCreateGroup()) and "a member must already
// be a real, independent AgentRecord before it can join" (see
// RequestJoinGroup()), so nothing else in this codebase has to re-derive
// either invariant.
//
// Deliberately narrow for 2.12E1/2.12E2: no automatic group formation, no
// cohesion/leader/policy logic, no combat/movement, no Loose-vs-Stable
// join/leave restriction yet (that is 2.12E3's job) - every call here is
// assumed to already be a deliberate, individually-authorized request
// (today: AIWorldMgr's own manual smoke test; later: an admin/test
// command). Nothing here ever touches Creature*/Player*/Map* - group
// lifecycle changes social relationships between already-existing
// AgentRecords, it never creates/destroys/moves/despawns anything in
// TrinityCore's own world state. World-thread-only, like everything else
// in AIWorld - both the synchronous validation each Request* method does
// up front AND every completion it later runs (see below) only ever
// happen on that thread.
//
// Milestone 2.12E2: every operation is Request*/async now, replacing
// 2.12E1's synchronous CreateGroup()/JoinGroup()/LeaveGroup()/
// DissolveGroup() (STATIC review: those blocked the calling thread on a
// DB round trip via AgentGroupPersistence's CONNECTION_SYNCH/
// DirectExecute() path - acceptable only for 2.12E1's own startup/admin-
// scoped caller, explicitly NOT acceptable once 2.12E3+ introduces an
// automatic or policy/AI-driven caller, e.g. a dynamically forming LOOSE
// coalition). Each Request* method:
//   1. Validates synchronously, in-memory only, against groupRegistry/
//      agentRegistry/_pendingGroupOperations (no DB) - group exists,
//      member exists, no duplicate, and (2.12E2 P2 fix, see below) no
//      other Join/Leave/Dissolve already in flight for this GroupId. A
//      validation failure calls onComplete synchronously, before
//      returning, and touches nothing else.
//   2. On success, marks groupId pending (see below) and calls into
//      AgentGroupPersistence's matching *Async() method, which submits
//      the DB work via CharacterDatabase.AsyncCommitTransaction() and
//      returns immediately - the calling (world) thread is never blocked
//      on the DB round trip.
//   3. Enqueues the returned TransactionCallback into the caller-supplied
//      TransactionCallbackProcessor - the caller (AIWorldMgr) owns polling
//      it once per world tick via ProcessReadyCallbacks(), the exact same
//      "enqueue, poll every Update()" shape World::_queryProcessor already
//      uses.
//   4. groupRegistry is mutated ONLY inside that completion, once
//      AgentGroupPersistence reports the transaction's own genuine commit
//      result - never optimistically, never before. Since the completion
//      runs later (possibly several ticks later), it never trusts that
//      anything found valid during step 1 is still valid at completion
//      time - it re-resolves groupRegistry.Find() itself rather than
//      reusing a pointer captured in step 1, and (2.12E2 P2 fix) it
//      unmarks groupId as pending before calling onComplete, regardless of
//      success or failure.
//
// Milestone 2.12E2 P2 fix (STATIC review): _pendingGroupOperations closes a
// real race an earlier version of this class had. RequestJoinGroup()/
// RequestLeaveGroup()/RequestDissolveGroup() validated against
// groupRegistry, which still contains the group right up until a DISSOLVE
// request's own completion erases it - so a JoinGroup request submitted
// while a DissolveGroup request for the same GroupId was already in
// flight (but not yet completed) would pass validation, submit its own
// INSERT, and could commit AFTER the dissolve's DELETE transaction already
// committed. ai_agent_group_members has no FK to ai_agent_groups by design
// (see AgentGroupPersistence::LoadGroupMembers()'s own comment on why),
// so nothing at the DB level would catch this - the result is a genuine
// orphan membership row naming a group_id that no longer exists, and
// groupRegistry.Find() returning null in the join's own completion is too
// late to prevent it, since the DB write already happened by then. Now:
// every Request* method (Create excepted - see below) checks
// _pendingGroupOperations first and rejects synchronously, before
// touching the DB at all, if the target GroupId already has an operation
// in flight; the completion always clears the entry before calling
// onComplete. This serializes at most one in-flight lifecycle operation
// per GroupId - not a queue that defers and later replays a rejected
// request, just a synchronous "busy, try again" the same way a duplicate
// join or an unknown group already is. A caller that needs a specific
// order (the manual smoke test; any future policy code) already has to
// chain each step from the previous one's onComplete to get a meaningful
// result anyway, so this never adds new caller-side complexity - it only
// forecloses the interleaving that could not have been useful in the
// first place. RequestCreateGroup() does not participate: it always mints
// a brand-new GroupId that cannot yet be known to (or racing against) any
// other caller, since nothing can resolve it in groupRegistry until this
// same class's own completion adds it there.
class TC_GAME_API AgentGroupLifecycleSystem
{
    public:
        // Synchronously nothing to validate here (persistence itself owns
        // the one thing that can reject a create - see
        // AgentGroupPersistence::CreateGroupAsync()'s fail-closed allocator
        // check). onComplete(groupId) fires once, either synchronously (a
        // rejection persistence detected before ever touching the DB) or
        // later from pending's ProcessReadyCallbacks() (once the DB
        // transaction's own result is known) - never both, never neither.
        // groupRegistry is only ever Add()'d to inside that completion,
        // after AgentGroupPersistence confirms the transaction actually
        // committed.
        //
        // Milestone 2.12E4C2 P2 fix (STATIC review): profileId names which
        // CoalitionFormationProfile (if any) is actually creating this
        // group - see AgentGroupRecord::ProfileId for why this is
        // persistent and separate from kind. Every caller must pass one
        // explicitly: CoalitionFormationProfileId::Invalid for a manual/
        // admin-authorized create (no automatic formation profile owns
        // this group, so no automatic maintenance profile may act on it
        // either), or the real profile's own Id for an automatic
        // RunCoalitionFormation() create. Threaded straight through to
        // AgentGroupPersistence::CreateGroupAsync() and the resulting
        // AgentGroupRecord unchanged.
        void RequestCreateGroup(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources,
            CoalitionFormationProfileId profileId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence, TransactionCallbackProcessor& pending,
            std::function<void(std::optional<GroupId>)> onComplete);

        // Validates synchronously before touching the DB at all: groupId
        // must resolve in groupRegistry, memberId must resolve in
        // agentRegistry (read-only - this never mutates an individual
        // AgentRecord), memberId must not already be a member of this
        // group, and (2.12E2 P2 fix) groupId must not already have another
        // Join/Leave/Dissolve in flight - see this class's own header
        // comment for why. Any failure calls onComplete(false)
        // synchronously and submits nothing. On success, marks groupId
        // pending, submits the async membership write, and only pushes
        // into AgentGroupRecord::Members inside the completion (which
        // always unmarks groupId first), once AgentGroupPersistence
        // confirms it - and only if groupId still resolves in
        // groupRegistry at that point (see this class's own header comment
        // on why a completion never trusts request-time validity).
        void RequestJoinGroup(GroupId groupId, AgentId memberId, uint64 joinedAtMs,
            AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry, AgentGroupPersistence& persistence,
            TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete);

        // Idempotent/fail-safe: an unknown groupId, a memberId that is not
        // currently a member, or (2.12E2 P2 fix) a groupId with another
        // Join/Leave/Dissolve already in flight all call onComplete(false)
        // synchronously (nothing to do, never an error) - calling this
        // twice in a row for the same (groupId, memberId) is always safe.
        // On success, marks groupId pending, submits the async removal,
        // and only erases from AgentGroupRecord::Members inside the
        // completion (which always unmarks groupId first), once
        // AgentGroupPersistence confirms it. If groupId no longer resolves
        // in groupRegistry by completion time (the group was dissolved
        // while this leave was in flight - no longer reachable via the
        // pending-operation guard above, but kept as defense in depth),
        // onComplete(true) still fires - the post-condition "not a member"
        // already holds either way, there is nothing left to erase.
        void RequestLeaveGroup(GroupId groupId, AgentId memberId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence,
            TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete);

        // An unknown groupId, or (2.12E2 P2 fix) a groupId with another
        // Join/Leave/Dissolve already in flight, calls onComplete(false)
        // synchronously, nothing to do. On success, marks groupId pending
        // and submits the async delete (every membership row for groupId,
        // then the group row itself, as one atomic transaction - see
        // AgentGroupPersistence::DeleteGroupAsync()), and only erases the
        // AgentGroupRecord from groupRegistry inside the completion (which
        // always unmarks groupId first), once that transaction's own
        // commit is confirmed - never touches AgentRegistry/AgentRecord/
        // Creature for any former member either way; they remain exactly
        // the ordinary individual agents they already were.
        void RequestDissolveGroup(GroupId groupId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence,
            TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete);

    private:
        // Milestone 2.12E2 P2 fix (STATIC review): GroupId::Value currently
        // has an in-flight Join/Leave/Dissolve request - see this class's
        // own header comment for the race this closes. Inserted by
        // RequestJoinGroup()/RequestLeaveGroup()/RequestDissolveGroup()
        // synchronously, right before each submits its async persistence
        // call; erased by that same request's completion, before
        // onComplete runs, regardless of success or failure.
        // RequestCreateGroup() never touches this - see this class's own
        // header comment for why it does not need to. World-thread-only,
        // like everything else here - what makes a plain
        // std::unordered_set safe without its own locking.
        std::unordered_set<uint64> _pendingGroupOperations;
};

#endif // AIWORLD_AGENTGROUPLIFECYCLESYSTEM_H
