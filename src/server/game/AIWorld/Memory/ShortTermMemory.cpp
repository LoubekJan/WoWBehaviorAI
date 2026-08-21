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

#include "ShortTermMemory.h"
#include "Log.h"
#include <algorithm>

MemoryRecord* ShortTermMemory::FindEquivalent(std::vector<MemoryRecord>& records, Observation const& observation) const
{
    for (MemoryRecord& record : records)
    {
        if (record.Type != observation.Type)
            continue;

        switch (observation.Type)
        {
            case ObservationType::WorldEvent:
                if (record.SourceEventId == observation.SourceEventId)
                    return &record;
                break;

            case ObservationType::PlayerSeen:
                if (record.Target.Guid == observation.Target.Guid)
                    return &record;
                break;

            case ObservationType::CreatureSeen:
                if (observation.Target.Agent)
                {
                    if (record.Target.Agent.Value == observation.Target.Agent.Value)
                        return &record;
                }
                else if (observation.Target.SpawnId)
                {
                    if (record.Location.MapId == observation.Location.MapId && record.Target.SpawnId == observation.Target.SpawnId)
                        return &record;
                }
                else if (record.Target.Guid == observation.Target.Guid)
                    return &record;
                break;
        }
    }

    return nullptr;
}

ShortTermMemory::RememberResult ShortTermMemory::Remember(Observation const& observation, uint64 ttlMs)
{
    std::vector<MemoryRecord>& records = _records[observation.Observer.Value];

    uint64 now = observation.ObservedAtMs;
    uint64 expiresAt = now + ttlMs;

    if (MemoryRecord* existing = FindEquivalent(records, observation))
    {
        existing->LastObservedAtMs = now;
        existing->ExpiresAtMs = expiresAt;
        ++existing->ObservationCount;

        existing->Location = observation.Location;
        existing->Actor = observation.Actor;
        existing->Target = observation.Target;
        existing->LastDistance = observation.Distance;
        existing->LastLineOfSight = observation.LineOfSight;

        TC_LOG_DEBUG("ai.world", "AI memory refreshed id={} agent={} type={} count={} targetGuid={}",
            existing->Id, observation.Observer.Value, ToString(existing->Type), existing->ObservationCount,
            existing->Target.Guid.ToString());

        return RememberResult::Refreshed;
    }

    if (records.size() >= MaxEntriesPerAgent)
    {
        auto oldest = std::min_element(records.begin(), records.end(),
            [](MemoryRecord const& a, MemoryRecord const& b) { return a.LastObservedAtMs < b.LastObservedAtMs; });

        TC_LOG_WARN("ai.world", "AI short-term memory capacity reached agent={}, evicting memory={}",
            observation.Observer.Value, oldest->Id);

        records.erase(oldest);
    }

    MemoryRecord record;
    record.Id = _nextMemoryId++;
    record.Owner = observation.Observer;
    record.Type = observation.Type;

    record.SourceEventId = observation.SourceEventId;
    record.CorrelationId = observation.CorrelationId;
    record.SourceOccurredAtMs = observation.SourceOccurredAtMs;
    record.SourceEventType = observation.SourceEventType;

    record.FirstObservedAtMs = now;
    record.LastObservedAtMs = now;
    record.ExpiresAtMs = expiresAt;
    record.ObservationCount = 1;

    record.Location = observation.Location;
    record.Actor = observation.Actor;
    record.Target = observation.Target;

    record.Channel = observation.Channel;
    record.LastDistance = observation.Distance;
    record.LastLineOfSight = observation.LineOfSight;

    if (record.Type == ObservationType::WorldEvent)
    {
        char const* sourceEventType = record.SourceEventType ? ToString(*record.SourceEventType) : "NONE";
        TC_LOG_DEBUG("ai.world", "AI memory added id={} agent={} type={} sourceEvent={} sourceEventType={}",
            record.Id, observation.Observer.Value, ToString(record.Type), record.SourceEventId, sourceEventType);
    }
    else
    {
        TC_LOG_DEBUG("ai.world", "AI memory added id={} agent={} type={} targetGuid={} ttl={}ms",
            record.Id, observation.Observer.Value, ToString(record.Type), record.Target.Guid.ToString(), ttlMs);
    }

    records.push_back(std::move(record));

    return RememberResult::Added;
}

void ShortTermMemory::Expire(uint64 nowMs)
{
    for (auto& entry : _records)
    {
        std::vector<MemoryRecord>& records = entry.second;

        for (auto it = records.begin(); it != records.end(); )
        {
            if (it->ExpiresAtMs <= nowMs)
            {
                TC_LOG_DEBUG("ai.world", "AI memory expired id={} agent={} type={} count={}",
                    it->Id, entry.first, ToString(it->Type), it->ObservationCount);
                it = records.erase(it);
            }
            else
                ++it;
        }
    }
}

std::vector<MemoryRecord> ShortTermMemory::GetForAgent(AgentId id) const
{
    auto it = _records.find(id.Value);
    if (it == _records.end())
        return {};

    return it->second;
}
