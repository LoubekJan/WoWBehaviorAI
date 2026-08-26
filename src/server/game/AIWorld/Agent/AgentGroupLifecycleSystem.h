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
#include "Define.h"
#include "GroupId.h"
#include "Persistence/TransactionCallbackProcessor.h"
#include <functional>
#include <optional>

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
// command). Pure value-transform-shaped otherwise: every dependency
// (registries, persistence, the pending-callback processor) is a
// parameter, nothing is held as member state, and nothing here ever
// touches Creature*/Player*/Map* - group lifecycle changes social
// relationships between already-existing AgentRecords, it never
// creates/destroys/moves/despawns anything in TrinityCore's own world
// state. World-thread-only, like everything else in AIWorld - both the
// synchronous validation each Request* method does up front AND every
// completion it later runs (see below) only ever happen on that thread.
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
//      agentRegistry (no DB) - group exists, member exists, no duplicate,
//      etc. A validation failure calls onComplete synchronously, before
//      returning, and touches nothing else.
//   2. On success, calls into AgentGroupPersistence's matching *Async()
//      method, which submits the DB work via
//      CharacterDatabase.AsyncCommitTransaction() and returns immediately
//      - the calling (world) thread is never blocked on the DB round
//      trip.
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
//      time - a group can be dissolved (or, once creation exists,
//      completed) while another request against the same GroupId is still
//      in flight, so every completion re-resolves groupRegistry.Find()
//      itself rather than reusing a pointer captured in step 1.
// No caller today issues two requests against the same GroupId
// concurrently (2.12E2 has no automatic policy yet, and the manual smoke
// test only ever chains one step's completion into the next), so that
// last point is a defense against a class of future bug, not a
// currently-reachable one - but the completion-time re-resolution costs
// nothing and is what keeps this safe once 2.12E3+ callers exist.
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
        void RequestCreateGroup(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence, TransactionCallbackProcessor& pending,
            std::function<void(std::optional<GroupId>)> onComplete) const;

        // Validates synchronously before touching the DB at all: groupId
        // must resolve in groupRegistry, memberId must resolve in
        // agentRegistry (read-only - this never mutates an individual
        // AgentRecord), and memberId must not already be a member of this
        // group. Any failure calls onComplete(false) synchronously and
        // submits nothing. On success, submits the async membership write
        // and only pushes into AgentGroupRecord::Members inside the
        // completion, once AgentGroupPersistence confirms it - and only
        // if groupId still resolves in groupRegistry at that point (see
        // this class's own header comment on why a completion never
        // trusts request-time validity).
        void RequestJoinGroup(GroupId groupId, AgentId memberId, uint64 joinedAtMs,
            AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry, AgentGroupPersistence& persistence,
            TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete) const;

        // Idempotent/fail-safe: an unknown groupId or a memberId that is
        // not currently a member both call onComplete(false) synchronously
        // (nothing to do, never an error) - calling this twice in a row for
        // the same (groupId, memberId) is always safe. On success, submits
        // the async removal and only erases from AgentGroupRecord::Members
        // inside the completion, once AgentGroupPersistence confirms it. If
        // groupId no longer resolves in groupRegistry by completion time
        // (the group was dissolved while this leave was in flight),
        // onComplete(true) still fires - the post-condition "not a member"
        // already holds either way, there is nothing left to erase.
        void RequestLeaveGroup(GroupId groupId, AgentId memberId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence,
            TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete) const;

        // An unknown groupId calls onComplete(false) synchronously, nothing
        // to do. On success, submits the async delete (every membership row
        // for groupId, then the group row itself, as one atomic
        // transaction - see AgentGroupPersistence::DeleteGroupAsync()) and
        // only erases the AgentGroupRecord from groupRegistry inside the
        // completion, once that transaction's own commit is confirmed -
        // never touches AgentRegistry/AgentRecord/Creature for any former
        // member either way; they remain exactly the ordinary individual
        // agents they already were.
        void RequestDissolveGroup(GroupId groupId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence,
            TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete) const;
};

#endif // AIWORLD_AGENTGROUPLIFECYCLESYSTEM_H
