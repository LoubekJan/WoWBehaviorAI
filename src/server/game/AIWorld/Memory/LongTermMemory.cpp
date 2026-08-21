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

#include "LongTermMemory.h"
#include <algorithm>

LongTermMemoryRecord* LongTermMemory::FindEquivalent(std::vector<LongTermMemoryRecord>& records, Observation const& observation) const
{
    for (LongTermMemoryRecord& record : records)
    {
        if (record.Type != observation.Type)
            continue;

        switch (observation.Type)
        {
            case ObservationType::WorldEvent:
                // Not just SourceEventId: EventBus's ids restart from 1
                // every process lifetime, so a bare id could otherwise
                // match an unrelated pre-restart memory.
                if (record.SourceEventId == observation.SourceEventId &&
                    record.SourceOccurredAtMs == observation.SourceOccurredAtMs)
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

std::optional<LongTermMemoryRecord> LongTermMemory::Remember(Observation const& observation, float importance)
{
    std::vector<LongTermMemoryRecord>& records = _records[observation.Observer.Value];

    if (LongTermMemoryRecord* existing = FindEquivalent(records, observation))
    {
        existing->LastObservedAtMs = observation.ObservedAtMs;
        ++existing->ObservationCount;
        existing->Importance = std::max(existing->Importance, importance);

        existing->Location = observation.Location;
        existing->Actor = observation.Actor;
        existing->Target = observation.Target;

        return std::nullopt;
    }

    LongTermMemoryRecord record;
    record.Owner = observation.Observer;
    record.Type = observation.Type;
    record.Importance = importance;

    record.SourceEventId = observation.SourceEventId;
    record.CorrelationId = observation.CorrelationId;
    record.SourceOccurredAtMs = observation.SourceOccurredAtMs;
    record.SourceEventType = observation.SourceEventType;

    record.FirstObservedAtMs = observation.ObservedAtMs;
    record.LastObservedAtMs = observation.ObservedAtMs;
    record.ObservationCount = 1;

    record.Location = observation.Location;
    record.Actor = observation.Actor;
    record.Target = observation.Target;

    record.Channel = observation.Channel;

    records.push_back(record);

    return record;
}

bool LongTermMemory::AddLoaded(LongTermMemoryRecord const& record)
{
    if (!record.PersistentId || !record.Owner)
        return false;

    _records[record.Owner.Value].push_back(record);
    return true;
}

std::vector<LongTermMemoryRecord> LongTermMemory::GetForAgent(AgentId id) const
{
    auto it = _records.find(id.Value);
    if (it == _records.end())
        return {};

    return it->second;
}
