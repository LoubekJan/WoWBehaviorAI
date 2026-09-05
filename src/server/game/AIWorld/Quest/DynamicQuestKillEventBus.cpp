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

#include "DynamicQuestKillEventBus.h"
#include "Log.h"

bool DynamicQuestKillEventBus::Publish(DynamicQuestKillEvent event)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_pending.size() >= MaxPendingEvents)
    {
        _droppedEvents.fetch_add(1, std::memory_order_relaxed);
        TC_LOG_ERROR("ai.world", "AI DynamicQuestKillEventBus queue full, dropping authoritative kill credit killer={} victim={}",
            event.KillerGuid.ToString(), event.VictimGuid.ToString());
        return false;
    }

    event.EventId = _nextEventId++;

    _pending.push_back(std::move(event));
    return true;
}

std::vector<DynamicQuestKillEvent> DynamicQuestKillEventBus::Drain()
{
    std::vector<DynamicQuestKillEvent> drained;

    std::lock_guard<std::mutex> lock(_mutex);

    drained.reserve(_pending.size());
    for (DynamicQuestKillEvent& event : _pending)
        drained.push_back(std::move(event));
    _pending.clear();

    return drained;
}
