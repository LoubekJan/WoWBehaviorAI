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

#ifndef AIWORLD_RETRIEVEDMEMORY_H
#define AIWORLD_RETRIEVEDMEMORY_H

#include "Define.h"
#include "Event/WorldEntityRef.h"
#include "Event/WorldEventType.h"
#include "Perception/ObservationType.h"
#include <optional>

enum class MemoryTier : uint8
{
    ShortTerm,
    LongTerm
};

inline char const* ToString(MemoryTier tier)
{
    switch (tier)
    {
        case MemoryTier::ShortTerm: return "SHORT_TERM";
        case MemoryTier::LongTerm:  return "LONG_TERM";
        default:                    return "UNKNOWN";
    }
}

// Normalized retrieval result - deliberately not MemoryRecord or
// LongTermMemoryRecord directly, so a future decision layer never needs
// to know the difference between the two tiers, only this one shape.
// MemoryId is the source record's Id (ShortTerm) or PersistentId
// (LongTerm) - only unique within its own Tier, never compare MemoryId
// across tiers without checking Tier too. Pure value object, same rule as
// everything else in AIWorld: no Creature*/Player*/Map* anywhere.
struct RetrievedMemory
{
    MemoryTier Tier = MemoryTier::ShortTerm;

    uint64 MemoryId = 0;

    ObservationType Type = ObservationType::WorldEvent;
    float Importance = 0.0f;
    float Relevance = 0.0f;

    uint64 SourceEventId = 0;
    uint64 SourceOccurredAtMs = 0;
    std::optional<WorldEventType> SourceEventType;

    uint64 FirstObservedAtMs = 0;
    uint64 LastObservedAtMs = 0;

    WorldEventLocation Location;
    WorldEntityRef Actor;
    WorldEntityRef Target;
};

#endif // AIWORLD_RETRIEVEDMEMORY_H
