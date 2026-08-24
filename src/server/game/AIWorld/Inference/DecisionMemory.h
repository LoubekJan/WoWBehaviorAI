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

#ifndef AIWORLD_DECISIONMEMORY_H
#define AIWORLD_DECISIONMEMORY_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Memory/RetrievedMemory.h"
#include <optional>

// Milestone 2.9A P2 fix: where a memory's WorldEventLocation happened -
// just a position, same fields as WorldEventLocation, kept as its own
// wire type so AgentContext's serialized shape doesn't silently change if
// WorldEventLocation ever grows an internal-only field.
struct DecisionLocation
{
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// Wire-safe stand-in for RetrievedMemory::Actor/Target (WorldEntityRef) -
// deliberately NOT a straight re-export of WorldEntityRef, which also
// carries ObjectGuid and SpawnId: those are internal engine/DB identity,
// not something ai-server needs or should be able to see. Entry (a
// creature/item template id, already public game data) and AgentId (0 if
// the entity isn't itself a tracked AIWorld agent, same "falsy" convention
// as AgentId::operator bool()) are the only parts of "who/what this was"
// that cross the wire.
struct DecisionEntity
{
    uint32 Entry = 0;
    AgentId Agent;
};

// Milestone 2.9A P2 fix: the actual wire shape for one of AgentContext's
// Top-N RelevantMemories - RetrievedMemory itself is an AIWorld-internal
// value (see RetrievedMemory.h) that still carries WorldEntityRef's raw
// Guid/SpawnId via Actor/Target; DecisionMemory is what AIWorldMgr
// sanitizes it down to before it ever reaches AgentContext. Everything
// RetrievedMemory's own metadata already offered (Tier/MemoryId/Type/
// Importance/Relevance/SourceEvent*/FirstObservedAtMs/LastObservedAtMs)
// is preserved as-is; only Location/Actor/Target are narrowed.
struct DecisionMemory
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

    DecisionLocation Location;
    DecisionEntity Actor;
    DecisionEntity Target;
};

// The one place RetrievedMemory's internal shape is narrowed down to what
// ai-server is allowed to see - called by AIWorldMgr::CaptureAndSubmitSnapshot()
// for every entry in this tick's Top-N MemoryRetrieval::Retrieve() result,
// so AgentContext::RelevantMemories is already wire-safe by construction
// and AIClient's JSON builder never has to make that judgment call itself.
inline DecisionMemory ToDecisionMemory(RetrievedMemory const& memory)
{
    DecisionMemory result;
    result.Tier = memory.Tier;
    result.MemoryId = memory.MemoryId;
    result.Type = memory.Type;
    result.Importance = memory.Importance;
    result.Relevance = memory.Relevance;
    result.SourceEventId = memory.SourceEventId;
    result.SourceOccurredAtMs = memory.SourceOccurredAtMs;
    result.SourceEventType = memory.SourceEventType;
    result.FirstObservedAtMs = memory.FirstObservedAtMs;
    result.LastObservedAtMs = memory.LastObservedAtMs;

    result.Location.MapId = memory.Location.MapId;
    result.Location.X = memory.Location.X;
    result.Location.Y = memory.Location.Y;
    result.Location.Z = memory.Location.Z;

    result.Actor.Entry = memory.Actor.Entry;
    result.Actor.Agent = memory.Actor.Agent;

    result.Target.Entry = memory.Target.Entry;
    result.Target.Agent = memory.Target.Agent;

    return result;
}

#endif // AIWORLD_DECISIONMEMORY_H
