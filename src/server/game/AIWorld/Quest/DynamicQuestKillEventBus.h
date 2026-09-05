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

#ifndef AIWORLD_DYNAMICQUESTKILLEVENTBUS_H
#define AIWORLD_DYNAMICQUESTKILLEVENTBUS_H

#include "DynamicQuestKillEvent.h"
#include "Define.h"
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

// Milestone 2.13C4 P2 fix (STATIC review): authoritative dynamic-quest
// kill credit must not share a transport with the shared, drop-under-
// overload perception/memory EventBus - dropping a CreatureKilled
// WorldEvent under EventBus::MaxPendingEvents pressure is an acceptable
// tradeoff for perception/memory (see EventBus's own comment), but is NOT
// acceptable for something the 2.13C roadmap requires to be authoritative:
// a real kill that never reaches DynamicQuestRegistry::ApplyProgress()
// leaves a player's own dynamic quest permanently short of a credit they
// actually earned, with no retry path. This bus exists purely to give
// that one narrow fact ("player X killed creature Y") its own capacity,
// isolated from however much unrelated perception/memory traffic EventBus
// is carrying at the same moment.
//
// Same thread-safe mutex + bounded deque + Drain() shape EventBus/
// ActionEngineEventBus already use. Unit::Kill() (a map-updater thread)
// calls Publish() only for a PLAYER killer (see its own call site comment
// - a non-player killer can never earn dynamic-quest credit, see
// AIWorldMgr::ProcessDynamicQuestKillProgress()'s own "direct-killer
// credit only" scope, so there is no reason to spend this bus's capacity
// on anything else), which by construction makes its real-world
// population a small fraction of EventBus's own (published for every
// creature death, any killer, any map). MaxPendingEvents is still a hard
// bound (never blocks or grows without limit waiting for the world thread
// to drain) but is set far larger than EventBus's own 4096 specifically
// because this bus never has to share that budget with any other event
// kind and each event here is tiny - this does not make a drop
// mathematically impossible, but combined with the much smaller real
// population it makes one vanishingly unlikely compared to sharing
// EventBus's own capacity with everything else AIWorld publishes.
class TC_GAME_API DynamicQuestKillEventBus
{
    public:
        // Assigns EventId under the same lock as the enqueue (not before
        // it - see EventBus::Publish()'s own comment for why that
        // ordering matters, even though this bus makes no ordering
        // promise of its own beyond FIFO). Returns false and drops the
        // event (logging once per drop, counted in
        // GetDroppedEventCount()) if the queue is already at
        // MaxPendingEvents - never blocks or grows without bound waiting
        // for the world thread to drain. A dropped event never consumes
        // an id.
        bool Publish(DynamicQuestKillEvent event);

        // World thread only. Empties the queue and returns everything
        // that was pending, in publish (== EventId) order.
        std::vector<DynamicQuestKillEvent> Drain();

        uint64 GetDroppedEventCount() const { return _droppedEvents.load(std::memory_order_relaxed); }

    private:
        static constexpr std::size_t MaxPendingEvents = 16384;

        // Only ever touched while holding _mutex, from inside Publish() -
        // not an atomic, the lock is what makes "assign under the same
        // lock as the push" possible at all.
        uint64 _nextEventId = 1;

        std::atomic<uint64> _droppedEvents{ 0 };

        std::mutex _mutex;
        std::deque<DynamicQuestKillEvent> _pending;
};

#endif // AIWORLD_DYNAMICQUESTKILLEVENTBUS_H
