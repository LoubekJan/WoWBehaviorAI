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

#include "EventBus.h"
#include "GameTime.h"
#include "Log.h"

bool EventBus::Publish(WorldEvent event)
{
    uint64 id = _nextEventId.fetch_add(1, std::memory_order_relaxed);
    event.EventId = id;
    if (event.CorrelationId == 0)
        event.CorrelationId = id;

    event.OccurredAtMs = uint64(GameTime::GetGameTime()) * 1000;

    std::lock_guard<std::mutex> lock(_mutex);

    if (_pending.size() >= MaxPendingEvents)
    {
        _droppedEvents.fetch_add(1, std::memory_order_relaxed);
        TC_LOG_WARN("ai.world", "AI EventBus queue full, dropping event type={}", ToString(event.Type));
        return false;
    }

    _pending.push_back(std::move(event));
    return true;
}

std::vector<WorldEvent> EventBus::Drain()
{
    std::vector<WorldEvent> drained;

    std::lock_guard<std::mutex> lock(_mutex);

    drained.reserve(_pending.size());
    for (WorldEvent& event : _pending)
        drained.push_back(std::move(event));
    _pending.clear();

    return drained;
}
