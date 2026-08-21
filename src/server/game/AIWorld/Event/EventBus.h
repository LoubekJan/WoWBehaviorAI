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

#ifndef AIWORLD_EVENTBUS_H
#define AIWORLD_EVENTBUS_H

#include "Define.h"
#include "WorldEvent.h"
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

// Thread-safe ingress for WorldEvents. Publish() is meant to be called from
// any thread that can observe a game-world fact - a map/combat worker,
// potentially concurrent with other maps' workers and with the world
// thread's own Drain() - and only ever does cheap, bounded, mutex-guarded
// work: it never touches Creature/Player/Map, never looks anything up in
// AgentRegistry, never calls ai-server. Drain() is for the world thread
// only, meant to be called once per tick after sMapMgr->Update(diff) has
// finished producing whatever it's going to produce that tick.
class TC_GAME_API EventBus
{
    public:
        // Assigns EventId under the same lock as the enqueue (not before
        // it), and CorrelationId too if the caller left it at 0 (a root
        // event). Returns false and drops the event without assigning it
        // an id (logging once per drop, counted in
        // GetDroppedEventCount()) if the queue is already at
        // MaxPendingEvents - never blocks or grows without bound waiting
        // for the world thread to drain. Guarantees queue order == EventId
        // order, which correlation/cause chains, audit, and replay all
        // depend on later.
        bool Publish(WorldEvent event);

        // World thread only. Empties the queue and returns everything that
        // was pending, in publish (== EventId) order.
        std::vector<WorldEvent> Drain();

        uint64 GetDroppedEventCount() const { return _droppedEvents.load(std::memory_order_relaxed); }

    private:
        static constexpr std::size_t MaxPendingEvents = 4096;

        // Only ever touched while holding _mutex, from inside Publish() -
        // not an atomic, the lock is what makes it safe (and is what makes
        // "assign under the same lock as the push" possible at all).
        uint64 _nextEventId = 1;

        // Unlike _nextEventId, this is read from GetDroppedEventCount()
        // without the lock (any thread), so it has to stay atomic even
        // though it's only ever written while _mutex is already held.
        std::atomic<uint64> _droppedEvents{ 0 };

        std::mutex _mutex;
        std::deque<WorldEvent> _pending;
};

#endif // AIWORLD_EVENTBUS_H
