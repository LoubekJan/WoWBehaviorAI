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
// mutated to match" (see CreateGroup()) and "a member must already be a
// real, independent AgentRecord before it can join" (see JoinGroup()),
// so nothing else in this codebase has to re-derive either invariant.
//
// Deliberately narrow for 2.12E1: no automatic group formation, no
// cohesion/leader/policy logic, no combat/movement, no Loose-vs-Stable
// join/leave restriction yet (that is 2.12E2's job) - every call here is
// assumed to already be a deliberate, individually-authorized request
// (today: AIWorldMgr's own manual smoke test; later: an admin/test
// command). Pure value-transform-shaped otherwise: every dependency
// (registries, persistence) is a parameter, nothing is held as member
// state, and nothing here ever touches Creature*/Player*/Map* - group
// lifecycle changes social relationships between already-existing
// AgentRecords, it never creates/destroys/moves/despawns anything in
// TrinityCore's own world state. World-thread-only, like everything else
// in AIWorld.
//
// Every method here only mutates groupRegistry (a create, a membership
// change, an erase) AFTER persistence has confirmed the matching DB write
// actually landed - STATIC review (round 2) on this class's first version
// found the opposite order for Join/Leave/Dissolve: they mutated
// groupRegistry right after an unconfirmed AgentGroupPersistence
// DirectExecute() call, so a DB failure there could leave groupRegistry
// disagreeing with what is actually stored. AgentGroupPersistence's own
// CreateGroup()/AddGroupMember()/RemoveGroupMember()/DeleteGroup() now all
// return a confirmed result (read-back after write, or - for
// DeleteGroup() - one atomic CharacterDatabaseTransaction plus a
// read-back); this class trusts nothing else.
//
// Milestone 2.12E1 P2 fix (STATIC review, round 2): every write below goes
// through AgentGroupPersistence's CONNECTION_SYNCH/DirectExecute() (and,
// for DeleteGroup(), DirectCommitTransaction()) path, which blocks the
// calling thread until the DB round-trip completes. That is an accepted,
// explicitly scoped tradeoff for 2.12E1 - every caller today is a rare,
// startup/admin-style action (AIWorldMgr's own manual smoke test), never a
// per-tick one. It is NOT acceptable once 2.12E2/2.12E3 introduce any
// automatic or policy/AI-driven caller of this same API - a dynamically
// forming LOOSE coalition calling CreateGroup()/JoinGroup() from a
// per-tick or per-decision path through this exact synchronous persistence
// layer would mean blocking DB I/O on the world thread, the same class of
// problem AgentEconomyState/AgentGroupRecord's own async
// SaveEconomyState()/SaveGroupState() were built specifically to avoid.
// Before any such caller exists, this persistence path must be redesigned
// - e.g. an async command queue that confirms via a callback rather than a
// blocking read-back, mirroring AIClient's own async request pattern -
// not wired in unchanged.
class TC_GAME_API AgentGroupLifecycleSystem
{
    public:
        // Mints a fresh GroupId via persistence.CreateGroup() (synchronous,
        // confirmed by read-back - see that method's own comment) and only
        // then adds the resulting AgentGroupRecord to groupRegistry -
        // never the other way around, so groupRegistry can never hold a
        // GroupId the DB does not actually have a row for. Returns
        // std::nullopt if persistence.CreateGroup() itself failed (already
        // logged there) or, defensively, if the freshly-minted id somehow
        // collided with an already-registered one (should be unreachable -
        // logged loudly if it ever happens, since it would mean
        // AgentGroupPersistence's own counter and groupRegistry have
        // drifted out of sync).
        std::optional<GroupId> CreateGroup(AgentGroupKind kind, uint32 territoryMapId, float territoryX, float territoryY, float territoryZ, float resources,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence) const;

        // Validates both sides before writing anything: groupId must
        // resolve in groupRegistry, memberId must resolve in agentRegistry
        // (read-only - this never mutates an individual AgentRecord), and
        // memberId must not already be a member of this group. Only once
        // all three hold does this write to persistence - and only once
        // persistence.AddGroupMember() confirms the write (2.12E1 P2 fix,
        // round 2) does it update groupRegistry, same order as
        // CreateGroup(). Returns false (and logs why) for any of the four
        // failures; true only on an actual, DB-confirmed join.
        bool JoinGroup(GroupId groupId, AgentId memberId, uint64 joinedAtMs,
            AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry, AgentGroupPersistence& persistence) const;

        // Idempotent/fail-safe: an unknown groupId or a memberId that is
        // not currently a member both return false (nothing to do), never
        // an error - calling this twice in a row for the same (groupId,
        // memberId) is always safe. Once a membership is found, this only
        // erases it from groupRegistry after persistence.RemoveGroupMember()
        // confirms the DB agrees (2.12E1 P2 fix, round 2) - a DB failure
        // here leaves the runtime membership untouched rather than
        // diverging from what is actually stored. Returns true only when a
        // membership actually existed and was confirmed removed.
        bool LeaveGroup(GroupId groupId, AgentId memberId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence) const;

        // Removes every membership row and the group row itself as one
        // atomic transaction (persistence.DeleteGroup()), and only erases
        // the AgentGroupRecord from groupRegistry once that confirms
        // success (2.12E1 P2 fix, round 2) - never touches AgentRegistry/
        // AgentRecord/Creature for any former member either way; they
        // remain exactly the ordinary individual agents they already were.
        // Returns false (nothing to do) for an unknown groupId, or if
        // persistence.DeleteGroup() itself could not confirm the DB rows
        // are actually gone (groupRegistry is left untouched in that case
        // too) - never a thrown error either way.
        bool DissolveGroup(GroupId groupId,
            AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence) const;
};

#endif // AIWORLD_AGENTGROUPLIFECYCLESYSTEM_H
