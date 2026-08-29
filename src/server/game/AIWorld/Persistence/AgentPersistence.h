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

#ifndef AIWORLD_AGENTPERSISTENCE_H
#define AIWORLD_AGENTPERSISTENCE_H

#include "Agent/AgentEconomyState.h"
#include "Agent/AgentId.h"
#include "Agent/AgentType.h"
#include "Define.h"

class AgentRegistry;

// Characters-DB-backed persistence for agent identity (the ai_agents
// table). Deliberately narrow for Milestone 2.2A: only AgentId/AgentType/
// MapId/SpawnId survive a restart. RuntimeGuid and WorldState are never
// written here - every agent LoadAgents() produces starts Abstract with an
// empty RuntimeGuid, regardless of what it was before shutdown, and
// SnapshotSequence is not persisted either. It only protects requests
// within one process's lifetime; no request from a previous run can
// legitimately still be in flight after a restart.
//
// characters DB was picked over world DB (static content, not realm
// runtime state) and over a fourth AI-specific database (not justified
// yet for one table). agent_id is MySQL AUTO_INCREMENT, not minted in
// C++: this database layer has no last-insert-id support, so
// CreateCreatureAgent() reads the assigned id back by the unique
// (map_id, spawn_id) binding instead of predicting it - deliberately not
// a locally-tracked counter, which would silently drift out of sync with
// the table the moment a row exists that LoadAgents() didn't see (a
// wider WHERE clause added later, another process, ...).
//
// Synchronous by design: LoadAgents() and CreateCreatureAgent() are only
// ever meant to be called during AIWorldMgr::Initialize(), never from the
// world update loop. SaveEconomyState() (2.11E2) is the one exception -
// see its own comment for why it is safe from there.
//
// Milestone 2.12D: AgentGroup persistence (ai_agent_groups/
// ai_agent_group_members) is no longer this class's job - see
// AgentGroupPersistence, its own dedicated class, now that a group is not
// an ai_agents row at all (see GroupId.h).
class TC_GAME_API AgentPersistence
{
    public:
        // Loads every row from ai_agents into registry (WorldState =
        // Abstract, RuntimeGuid = Empty, SnapshotSequence = 0). Returns the
        // number of agents loaded.
        uint32 LoadAgents(AgentRegistry& registry);

        // Idempotent against ai_agents itself (not just against what
        // LoadAgents() already saw): if a row for (mapId, spawnId) already
        // exists, its AgentId is reused instead of inserting a duplicate.
        // Otherwise inserts a new row and reads its MySQL-assigned AgentId
        // back by that same binding. Returns AgentId{} (Value == 0, never a
        // valid id) if the read-back doesn't find a row - the caller must
        // not add anything to an AgentRegistry in that case, since there is
        // then no way to know whether the insert actually happened.
        AgentId CreateCreatureAgent(AgentType type, uint32 mapId, uint64 spawnId);

        // Milestone 2.12F4A P2 fix (STATIC review): explicit, synchronous
        // ControlMode upgrade for a single already-created agent - startup-
        // only, like CreateCreatureAgent()/LoadAgents() above, never called
        // from the world update loop. CreateCreatureAgent() itself
        // deliberately never sets control_mode, always taking the schema's
        // own ObserveOnly (0) DEFAULT (see CHAR_INS_AI_AGENT's own
        // comment) - correct for every general caller, but wrong for
        // AIWorld.TestSpawnId's own "explicitly chosen AIWorld test agent"
        // fixture, which must become AIWorldControlled the moment it is
        // freshly created, not silently stay at the generic default. A
        // caller that legitimately needs a just-created agent to start
        // AIWorldControlled calls this as its own explicit follow-up step
        // - never something CreateCreatureAgent() infers from a hint or a
        // default parameter.
        //
        // Milestone 2.12F4A P2 fix (STATIC review, round 2): returns
        // whether the UPDATE was actually confirmed by a read-back, not
        // just issued. DirectExecute() itself never reports success/
        // failure (see CreateCreatureAgent()'s own comment on why its
        // INSERT needs a read-back) - the same is true for an UPDATE, so a
        // caller must not set an in-memory AgentRecord::ControlMode to
        // AIWorldControlled, nor treat ownership as granted, until this
        // returns true. On false (row missing, or the DB write silently
        // didn't land) the caller must fail closed and leave the agent at
        // whatever ControlMode it already had in memory.
        bool SetControlMode(AgentId id, AgentControlMode mode);

        // Milestone 2.11E2: fire-and-forget async UPDATE (CONNECTION_ASYNC/
        // Execute(), never CONNECTION_SYNCH/DirectExecute()) - unlike
        // LoadAgents()/CreateCreatureAgent(), this is meant to be called
        // from the world update loop (right after AIWorldMgr mutates
        // AgentRecord::EconomyState in memory) and must never block it on
        // the DB. No read-back/result to report: the in-memory
        // AgentRecord::EconomyState the caller already updated is
        // authoritative for this process's remaining lifetime regardless
        // of whether/when the write actually lands.
        //
        // Milestone 2.11E2 P3 fix: state is a mutable reference, not const
        // - this function increments state.Version itself, unconditionally,
        // as its first step, before persisting. That used to be the
        // caller's job (AIWorldMgr::MutateEconomyAndPersist()), but a
        // convention only holds for callers that go through it; anyone
        // calling SaveEconomyState() directly could still forget the bump
        // and the UPDATE's own "AND economy_version < ?" guard would then
        // silently reject the write. Bumping here instead means every
        // caller that actually persists gets it for free - there is no
        // path to a real DB write that skips it.
        void SaveEconomyState(AgentId id, AgentEconomyState& state);

    private:
        AgentId FindBinding(uint32 mapId, uint64 spawnId);
};

#endif // AIWORLD_AGENTPERSISTENCE_H
