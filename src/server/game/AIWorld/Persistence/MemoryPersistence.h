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

#ifndef AIWORLD_MEMORYPERSISTENCE_H
#define AIWORLD_MEMORYPERSISTENCE_H

#include "Define.h"

class AgentRegistry;
class LongTermMemory;
struct LongTermMemoryRecord;

// Characters-DB-backed persistence for promoted long-term memories (the
// ai_long_term_memories table). Deliberately separate from
// AgentPersistence, which is only for agent identity/startup sync:
// long-term memory promotion happens continuously while the world is
// running, not just at startup, so it needs a different threading
// contract. LoadLongTermMemories() is synchronous and startup-only, like
// AgentPersistence's LoadAgents(). PersistLongTermMemory() is
// fire-and-forget async (CONNECTION_ASYNC/Execute()), called from the
// world update thread whenever a memory is promoted - it must never
// block that thread on the database the way AgentPersistence's
// DirectExecute() would.
class TC_GAME_API MemoryPersistence
{
    public:
        // Loads every row from ai_long_term_memories into memory. Skips
        // (logging a WARN, never crashing) any row whose agent_id isn't
        // found in registry - an orphan can happen (e.g. manual DB
        // editing, a future agent-deletion feature) and must not be
        // fatal. Returns the number of records actually loaded.
        uint32 LoadLongTermMemories(LongTermMemory& memory, AgentRegistry const& registry);

        // Fire-and-forget: queues an async INSERT and returns immediately,
        // never blocking the calling (world update) thread. Does not, and
        // by design cannot, report the row's assigned memory_id back;
        // that's only ever observed later, via a subsequent
        // LoadLongTermMemories() call (i.e. after a restart).
        void PersistLongTermMemory(LongTermMemoryRecord const& record);
};

#endif // AIWORLD_MEMORYPERSISTENCE_H
