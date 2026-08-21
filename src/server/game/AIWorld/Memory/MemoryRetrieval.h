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

#ifndef AIWORLD_MEMORYRETRIEVAL_H
#define AIWORLD_MEMORYRETRIEVAL_H

#include "Define.h"
#include "LongTermMemoryRecord.h"
#include "MemoryQueryContext.h"
#include "MemoryRecord.h"
#include "RetrievedMemory.h"
#include <cstddef>
#include <vector>

// Deterministic relevance scoring, cross-tier dedupe, and Top-N selection -
// no LLM, no randomness. A pure function of its inputs: never touches
// ShortTermMemory/LongTermMemory itself (the caller is responsible for
// already having a per-agent, non-expired short-term slice - see
// ShortTermMemory::GetActiveForAgent() - and a per-agent long-term slice),
// never touches Creature/Player/Map, never calls ai-server. Not itself a
// decision context - this milestone (2.5C) only proves retrieval works;
// nothing consumes the result yet.
class TC_GAME_API MemoryRetrieval
{
    public:
        // relevance = 0.55*importance + 0.25*freshness + 0.20*locality,
        // all three components already 0.0-1.0. A ShortTerm and a
        // LongTerm record for the same underlying thing (same semantic
        // identity ShortTermMemory/LongTermMemory already use for their
        // own dedup: WorldEvent by (SourceEventId, SourceOccurredAtMs),
        // PlayerSeen by Target.Guid, CreatureSeen by Target.Agent then
        // (MapId, SpawnId) then Target.Guid) collapse into one result -
        // the higher-relevance copy wins, LongTerm breaking a tie.
        // Sorted by relevance descending, then Importance, then
        // LastObservedAtMs, then Tier (LongTerm first), then MemoryId, and
        // truncated to maxResults.
        std::vector<RetrievedMemory> Retrieve(MemoryQueryContext const& context,
            std::vector<MemoryRecord> const& shortTerm, std::vector<LongTermMemoryRecord> const& longTerm,
            std::size_t maxResults) const;

    private:
        float ScoreShortTerm(MemoryQueryContext const& context, MemoryRecord const& memory) const;
        float ScoreLongTerm(MemoryQueryContext const& context, LongTermMemoryRecord const& memory) const;

        // Shared by both: 0 if off-map, else 1 at distance 0 falling
        // linearly to 0 at LocalityRange yards.
        float ScoreLocality(MemoryQueryContext const& context, WorldEventLocation const& location) const;

        bool IsSameMemory(RetrievedMemory const& a, RetrievedMemory const& b) const;
};

#endif // AIWORLD_MEMORYRETRIEVAL_H
