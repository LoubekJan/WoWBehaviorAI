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

#ifndef AIWORLD_LONGTERMMEMORY_H
#define AIWORLD_LONGTERMMEMORY_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "LongTermMemoryRecord.h"
#include "Perception/Observation.h"
#include <optional>
#include <unordered_map>
#include <vector>

// World-thread-only, in-memory index of promoted long-term memories -
// mirrors (once loaded/persisted) characters.ai_long_term_memories, but
// is not itself the persistence layer; it never touches the database
// (see MemoryPersistence for that). Pure value storage: never touches
// Creature/Player/Map, never calls ai-server, never mutates world state.
class TC_GAME_API LongTermMemory
{
    public:
        // Finds an existing LongTermMemoryRecord owned by
        // observation.Observer for the same underlying thing and
        // refreshes it in place (LastObservedAtMs, ObservationCount,
        // Importance = max(existing, new), Location, Actor, Target),
        // returning nullopt - a refresh has nothing new to persist.
        // Otherwise creates, stores, and returns a new record; the
        // caller is responsible for persisting it, this class never
        // touches the database itself.
        //
        // Matching rule: same as ShortTermMemory's, except WorldEvent
        // matches on (SourceEventId, SourceOccurredAtMs) together, not
        // SourceEventId alone - EventBus's ids restart from 1 every
        // process lifetime, so a bare SourceEventId could otherwise
        // collide with an unrelated long-term memory from before a
        // restart that happened to reuse the same id.
        std::optional<LongTermMemoryRecord> Remember(Observation const& observation, float importance);

        // Inserts an already-persisted record (from MemoryPersistence,
        // loaded at startup) into the in-memory index. Rejects (returns
        // false) a record with PersistentId == 0 or Owner == AgentId{} -
        // both should be impossible for a row actually loaded from
        // ai_long_term_memories. Does not itself touch the database.
        bool AddLoaded(LongTermMemoryRecord const& record);

        std::vector<LongTermMemoryRecord> GetForAgent(AgentId id) const;

    private:
        LongTermMemoryRecord* FindEquivalent(std::vector<LongTermMemoryRecord>& records, Observation const& observation) const;

        std::unordered_map<uint64, std::vector<LongTermMemoryRecord>> _records;
};

#endif // AIWORLD_LONGTERMMEMORY_H
