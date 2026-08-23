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

#ifndef AIWORLD_ACTIONENGINEEVENTBUS_H
#define AIWORLD_ACTIONENGINEEVENTBUS_H

#include "ActionEngineEvent.h"
#include "Define.h"
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

// Thread-safe ingress for ActionEngineEvents, the same mutex + bounded
// queue + Drain() pattern EventBus already uses for WorldEvents - kept as
// its own class rather than reusing EventBus because an engine movement-
// completion callback is not a perception event and has no use for
// EventId/CorrelationId/cause chains. Publish() is meant to be called from
// AIWorldCreatureAI::MovementInform(), on whatever thread TrinityCore
// itself calls that from (a map-updater thread during Map::Update(), the
// same context FactorySelector::SelectAI() runs in) - it never touches
// Creature/AgentRegistry, never calls ai-server. Drain() is for the world
// thread only.
class TC_GAME_API ActionEngineEventBus
{
    public:
        // Returns false and drops the event (logging once per drop,
        // counted in GetDroppedEventCount()) if the queue is already at
        // MaxPendingEvents - never blocks or grows without bound waiting
        // for the world thread to drain.
        bool Publish(ActionEngineEvent event);

        // World thread only. Empties the queue and returns everything that
        // was pending, in publish order.
        std::vector<ActionEngineEvent> Drain();

        uint64 GetDroppedEventCount() const { return _droppedEvents.load(std::memory_order_relaxed); }

    private:
        static constexpr std::size_t MaxPendingEvents = 4096;

        // Unlike WorldEvent's EventId counter, this has no ordering
        // contract to preserve beyond FIFO, so it stays a simple atomic
        // rather than needing to be assigned under the same lock as the
        // push.
        std::atomic<uint64> _droppedEvents{ 0 };

        std::mutex _mutex;
        std::deque<ActionEngineEvent> _pending;
};

#endif // AIWORLD_ACTIONENGINEEVENTBUS_H
