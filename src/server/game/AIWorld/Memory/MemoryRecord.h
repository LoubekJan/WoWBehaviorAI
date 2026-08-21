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

#ifndef AIWORLD_MEMORYRECORD_H
#define AIWORLD_MEMORYRECORD_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEntityRef.h"
#include "Event/WorldEventType.h"
#include "Perception/ObservationType.h"
#include "Perception/PerceptionChannel.h"
#include <optional>

// What an agent currently remembers - a deduplicated, TTL'd summary of one
// or more Observations about the same underlying thing (see
// ShortTermMemory::Remember() for the matching rule), not a 1:1 log of
// every Observation. Pure value object, same rule as Observation/
// WorldEvent/AgentRecord: no Creature*/Player*/Map* anywhere.
struct MemoryRecord
{
    uint64 Id = 0;
    AgentId Owner;

    ObservationType Type = ObservationType::WorldEvent;

    // MemoryImportance::Score() at the time this record was created; on a
    // refresh, the max of the existing value and the new Observation's
    // score - importance never drops just because a later Observation of
    // the same thing happened to score lower.
    float Importance = 0.0f;

    uint64 SourceEventId = 0;
    uint64 CorrelationId = 0;
    uint64 SourceOccurredAtMs = 0;
    std::optional<WorldEventType> SourceEventType;

    uint64 FirstObservedAtMs = 0;
    uint64 LastObservedAtMs = 0;
    uint64 ExpiresAtMs = 0;

    uint32 ObservationCount = 0;

    WorldEventLocation Location;
    WorldEntityRef Actor;
    WorldEntityRef Target;

    PerceptionChannel Channel = PerceptionChannel::Sight;

    float LastDistance = 0.0f;
    bool LastLineOfSight = false;
};

#endif // AIWORLD_MEMORYRECORD_H
