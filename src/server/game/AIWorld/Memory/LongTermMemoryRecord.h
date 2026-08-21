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

#ifndef AIWORLD_LONGTERMMEMORYRECORD_H
#define AIWORLD_LONGTERMMEMORYRECORD_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEntityRef.h"
#include "Event/WorldEventType.h"
#include "Perception/ObservationType.h"
#include "Perception/PerceptionChannel.h"
#include <optional>

// A promoted, important-enough-to-persist memory. Deliberately not
// MemoryRecord reused: short-term has ExpiresAtMs, long-term doesn't -
// forgetting a long-term memory is a later, deliberate decision (not this
// milestone's), not a timer. Pure value object, same rule as everything
// else in AIWorld: no Creature*/Player*/Map* anywhere.
struct LongTermMemoryRecord
{
    // characters.ai_long_term_memories.memory_id (AUTO_INCREMENT). 0 for a
    // record not yet known to be persisted - a new one this process just
    // created, before MemoryPersistence's async INSERT reports back (it
    // doesn't, by design; see MemoryPersistence).
    uint64 PersistentId = 0;

    AgentId Owner;

    ObservationType Type = ObservationType::WorldEvent;
    float Importance = 0.0f;

    uint64 SourceEventId = 0;
    uint64 CorrelationId = 0;
    uint64 SourceOccurredAtMs = 0;
    std::optional<WorldEventType> SourceEventType;

    uint64 FirstObservedAtMs = 0;
    uint64 LastObservedAtMs = 0;
    uint32 ObservationCount = 1;

    WorldEventLocation Location;
    WorldEntityRef Actor;
    WorldEntityRef Target;

    PerceptionChannel Channel = PerceptionChannel::Sight;
};

#endif // AIWORLD_LONGTERMMEMORYRECORD_H
