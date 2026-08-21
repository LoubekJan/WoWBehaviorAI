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

#ifndef AIWORLD_SHORTTERMMEMORY_H
#define AIWORLD_SHORTTERMMEMORY_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "MemoryRecord.h"
#include "Perception/Observation.h"
#include <cstddef>
#include <unordered_map>
#include <vector>

// World-thread-only: an in-memory, per-agent, TTL'd, deduplicated record of
// Observations. Milestone 2.5A/2.5B1 - not persisted (2.5B2+), not queried
// by decision context yet (2.5C). Pure value storage: never touches
// Creature/Player/Map, never calls ai-server, never mutates world state.
// Logs its own Added/Refreshed/Expired/capacity-eviction transitions, the
// same way AgentRegistry and EventBus already log their own state changes.
class TC_GAME_API ShortTermMemory
{
    public:
        enum class RememberResult
        {
            Added,
            Refreshed
        };

        // Finds an existing MemoryRecord owned by observation.Observer for
        // the same underlying thing and refreshes it (LastObservedAtMs,
        // ExpiresAtMs, ObservationCount, Location, Actor, Target,
        // LastDistance, LastLineOfSight all updated to the new
        // Observation's values; Importance set to max(existing, importance)
        // - it never drops just because a later Observation scored lower),
        // or creates a new one if none matches. "Now" is taken from
        // observation.ObservedAtMs, never queried separately.
        //
        // Matching rule, by observation.Type:
        //   WorldEvent   -> same SourceEventId
        //   PlayerSeen   -> same Target.Guid
        //   CreatureSeen -> same Target.Agent if the observation has one
        //                   (Target.Agent is truthy), else same
        //                   (Location.MapId, Target.SpawnId) if SpawnId
        //                   != 0, else same Target.Guid - SpawnId is
        //                   preferred over the runtime Guid because a
        //                   Creature's GUID can change across a grid
        //                   reload while its SpawnId stays stable.
        //
        // If the owning agent is already at MaxEntriesPerAgent and this is
        // a new (non-matching) memory, the oldest record by
        // LastObservedAtMs is evicted first.
        RememberResult Remember(Observation const& observation, uint64 ttlMs, float importance);

        // World thread only, meant to be called on its own ~1s maintenance
        // cadence, not every tick. Removes every record (for every agent)
        // whose ExpiresAtMs <= nowMs.
        void Expire(uint64 nowMs);

        std::vector<MemoryRecord> GetForAgent(AgentId id) const;

        // Same as GetForAgent(), but excludes anything already past its
        // ExpiresAtMs. Expire() only runs on its own ~1s maintenance
        // cadence, so a record can briefly sit expired-but-not-yet-swept;
        // retrieval must never surface one of those. Use this, never the
        // unfiltered GetForAgent(), for anything that feeds a decision or
        // ranking (see MemoryRetrieval).
        std::vector<MemoryRecord> GetActiveForAgent(AgentId id, uint64 nowMs) const;

    private:
        static constexpr std::size_t MaxEntriesPerAgent = 128;

        MemoryRecord* FindEquivalent(std::vector<MemoryRecord>& records, Observation const& observation) const;

        uint64 _nextMemoryId = 1;

        std::unordered_map<uint64, std::vector<MemoryRecord>> _records;
};

#endif // AIWORLD_SHORTTERMMEMORY_H
