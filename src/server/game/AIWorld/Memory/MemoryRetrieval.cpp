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

#include "MemoryRetrieval.h"
#include <algorithm>
#include <cmath>

float MemoryRetrieval::ScoreLocality(MemoryQueryContext const& context, WorldEventLocation const& location) const
{
    if (location.MapId != context.MapId)
        return 0.0f;

    float dx = location.X - context.X;
    float dy = location.Y - context.Y;
    float dz = location.Z - context.Z;

    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    constexpr float LocalityRange = 200.0f;
    return 1.0f - std::clamp(distance / LocalityRange, 0.0f, 1.0f);
}

float MemoryRetrieval::ScoreShortTerm(MemoryQueryContext const& context, MemoryRecord const& memory) const
{
    // ExpiresAtMs is always LastObservedAtMs + ttlMs (see
    // ShortTermMemory::Remember()), so lifetime is really just ttlMs -
    // computed from the two stored timestamps instead of needing its own
    // config value here.
    uint64 lifetime = memory.ExpiresAtMs - memory.LastObservedAtMs;
    uint64 remaining = memory.ExpiresAtMs > context.NowMs ? memory.ExpiresAtMs - context.NowMs : 0;

    float freshness = lifetime ? std::clamp(float(remaining) / float(lifetime), 0.0f, 1.0f) : 0.0f;
    float locality = ScoreLocality(context, memory.Location);

    return 0.55f * memory.Importance + 0.25f * freshness + 0.20f * locality;
}

float MemoryRetrieval::ScoreLongTerm(MemoryQueryContext const& context, LongTermMemoryRecord const& memory) const
{
    // No TTL for long-term memory, so freshness is a slow deterministic
    // decay instead: ~1.00 now, ~0.50 at a day old, ~0.125 at a week old.
    // Nothing is ever deleted because of this - it only affects ranking.
    constexpr double DayMs = 24.0 * 60.0 * 60.0 * 1000.0;

    double ageMs = context.NowMs > memory.LastObservedAtMs ? double(context.NowMs - memory.LastObservedAtMs) : 0.0;
    float freshness = float(1.0 / (1.0 + ageMs / DayMs));

    float locality = ScoreLocality(context, memory.Location);

    return 0.55f * memory.Importance + 0.25f * freshness + 0.20f * locality;
}

bool MemoryRetrieval::IsSameMemory(RetrievedMemory const& a, RetrievedMemory const& b) const
{
    if (a.Type != b.Type)
        return false;

    switch (a.Type)
    {
        case ObservationType::WorldEvent:
            // Not just SourceEventId: EventBus's ids restart from 1 every
            // process lifetime, so a bare id could otherwise match an
            // unrelated pre-restart long-term memory.
            return a.SourceEventId == b.SourceEventId && a.SourceOccurredAtMs == b.SourceOccurredAtMs;

        case ObservationType::PlayerSeen:
            return a.Target.Guid == b.Target.Guid;

        case ObservationType::CreatureSeen:
            if (a.Target.Agent.Value != 0 || b.Target.Agent.Value != 0)
                return a.Target.Agent.Value == b.Target.Agent.Value;

            if (a.Target.SpawnId != 0 || b.Target.SpawnId != 0)
                return a.Location.MapId == b.Location.MapId && a.Target.SpawnId == b.Target.SpawnId;

            return a.Target.Guid == b.Target.Guid;
    }

    return false;
}

std::vector<RetrievedMemory> MemoryRetrieval::Retrieve(MemoryQueryContext const& context,
    std::vector<MemoryRecord> const& shortTerm, std::vector<LongTermMemoryRecord> const& longTerm,
    std::size_t maxResults) const
{
    std::vector<RetrievedMemory> combined;
    combined.reserve(shortTerm.size() + longTerm.size());

    for (MemoryRecord const& memory : shortTerm)
    {
        RetrievedMemory retrieved;
        retrieved.Tier = MemoryTier::ShortTerm;
        retrieved.MemoryId = memory.Id;
        retrieved.Type = memory.Type;
        retrieved.Importance = memory.Importance;
        retrieved.Relevance = ScoreShortTerm(context, memory);
        retrieved.SourceEventId = memory.SourceEventId;
        retrieved.SourceOccurredAtMs = memory.SourceOccurredAtMs;
        retrieved.SourceEventType = memory.SourceEventType;
        retrieved.FirstObservedAtMs = memory.FirstObservedAtMs;
        retrieved.LastObservedAtMs = memory.LastObservedAtMs;
        retrieved.Location = memory.Location;
        retrieved.Actor = memory.Actor;
        retrieved.Target = memory.Target;

        combined.push_back(retrieved);
    }

    for (LongTermMemoryRecord const& memory : longTerm)
    {
        RetrievedMemory retrieved;
        retrieved.Tier = MemoryTier::LongTerm;
        retrieved.MemoryId = memory.PersistentId;
        retrieved.Type = memory.Type;
        retrieved.Importance = memory.Importance;
        retrieved.Relevance = ScoreLongTerm(context, memory);
        retrieved.SourceEventId = memory.SourceEventId;
        retrieved.SourceOccurredAtMs = memory.SourceOccurredAtMs;
        retrieved.SourceEventType = memory.SourceEventType;
        retrieved.FirstObservedAtMs = memory.FirstObservedAtMs;
        retrieved.LastObservedAtMs = memory.LastObservedAtMs;
        retrieved.Location = memory.Location;
        retrieved.Actor = memory.Actor;
        retrieved.Target = memory.Target;

        combined.push_back(retrieved);
    }

    // Dedupe: the same underlying fact can exist in both tiers (a
    // CREATURE_KILLED important enough to promote is still also in
    // short-term). O(n^2) - fine at the scale one agent's memories reach
    // (short-term capped at 128; long-term not yet capped, but not
    // expected to be large before a future consolidation milestone).
    std::vector<RetrievedMemory> deduped;
    deduped.reserve(combined.size());

    for (RetrievedMemory const& candidate : combined)
    {
        bool merged = false;
        for (RetrievedMemory& existing : deduped)
        {
            if (!IsSameMemory(candidate, existing))
                continue;

            bool candidateWins = candidate.Relevance > existing.Relevance ||
                (candidate.Relevance == existing.Relevance &&
                    candidate.Tier == MemoryTier::LongTerm && existing.Tier != MemoryTier::LongTerm);

            if (candidateWins)
                existing = candidate;

            merged = true;
            break;
        }

        if (!merged)
            deduped.push_back(candidate);
    }

    std::sort(deduped.begin(), deduped.end(), [](RetrievedMemory const& a, RetrievedMemory const& b)
    {
        if (a.Relevance != b.Relevance)
            return a.Relevance > b.Relevance;

        if (a.Importance != b.Importance)
            return a.Importance > b.Importance;

        if (a.LastObservedAtMs != b.LastObservedAtMs)
            return a.LastObservedAtMs > b.LastObservedAtMs;

        if (a.Tier != b.Tier)
            return a.Tier == MemoryTier::LongTerm;

        return a.MemoryId < b.MemoryId;
    });

    if (deduped.size() > maxResults)
        deduped.resize(maxResults);

    return deduped;
}
