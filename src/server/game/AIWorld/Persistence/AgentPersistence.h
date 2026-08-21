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
// yet for one table). AgentId is assigned here, in C++, not by MySQL
// AUTO_INCREMENT - this database layer has no last-insert-id support, and
// TrinityCore's own convention is to mint ids in code rather than rely on
// one.
//
// Synchronous by design: LoadAgents() and CreateCreatureAgent() are only
// ever meant to be called during AIWorldMgr::Initialize(), never from the
// world update loop.
class TC_GAME_API AgentPersistence
{
    public:
        // Loads every enabled row from ai_agents into registry (WorldState
        // = Abstract, RuntimeGuid = Empty, SnapshotSequence = 0) and seeds
        // the id counter CreateCreatureAgent() hands out next. Returns the
        // number of agents loaded.
        uint32 LoadAgents(AgentRegistry& registry);

        // Idempotent against ai_agents (not just against what LoadAgents()
        // already saw - a disabled row for the same binding would otherwise
        // collide with the UNIQUE(map_id, spawn_id) constraint): if a row
        // for (mapId, spawnId) already exists, its AgentId is reused
        // instead of inserting a duplicate. Otherwise inserts a new row and
        // returns the AgentId it was assigned - the only place a new
        // AgentId is minted. Caller is responsible for adding the
        // resulting record to an AgentRegistry.
        AgentId CreateCreatureAgent(AgentType type, uint32 mapId, uint64 spawnId);

    private:
        uint64 _nextAgentId = 1;
};

#endif // AIWORLD_AGENTPERSISTENCE_H
