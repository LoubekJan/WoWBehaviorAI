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

#ifndef AIWORLD_DYNAMICQUESTKILLEVENT_H
#define AIWORLD_DYNAMICQUESTKILLEVENT_H

#include "Define.h"
#include "ObjectGuid.h"

// Milestone 2.13C4 P2 fix (STATIC review): a pure value fact, exactly
// like WorldEvent (see its own comment) - no Creature*/Player*/Unit*
// anywhere, so it can safely cross from whatever map-updater thread
// Unit::Kill() runs on to the world thread through
// DynamicQuestKillEventBus without any of those pointers' lifetimes
// mattering. Deliberately NOT a reuse of WorldEvent - see
// DynamicQuestKillEventBus's own comment for why authoritative dynamic-
// quest kill credit needs its own transport, separate from the shared,
// drop-under-overload perception/memory EventBus.
struct DynamicQuestKillEvent
{
    // Assigned by DynamicQuestKillEventBus::Publish(), not by whoever
    // constructs this - leave at 0. Unique only within this bus; no
    // relation to WorldEvent::EventId's own counter.
    // DynamicQuestRegistry::ApplyProgress()'s replay guard
    // (ConsumedProgressEventIds) is scoped per DynamicQuestInstance, not
    // globally, so the two counters never need to agree with each other.
    uint64 EventId = 0;

    ObjectGuid KillerGuid;

    // Milestone 2.13C4 P2 fix (STATIC review, round 3): VictimGuid alone
    // is kept only as an informational/audit value now - progress
    // matching uses VictimEntry+MapId below, not this. A dynamic quest's
    // RequiredCount > 1 cannot generally be satisfied by requiring the
    // exact same runtime spawn to die repeatedly (it may never respawn,
    // or not before the quest's own ExpiryMs); DynamicQuestCreation.cpp
    // already establishes VictimGuid as authoritative PROVENANCE only
    // (proving the model's proposal was actually about a real, live
    // target at creation time - see its own comment), while
    // TargetEntry/TargetMapId are what the accepted quest's own kill-
    // count OBJECTIVE is defined against, the same split
    // DynamicQuestCreation.cpp's own target.Entry/target.MapId re-check
    // already uses.
    ObjectGuid VictimGuid;
    uint32 VictimEntry = 0;
    uint32 MapId = 0;
};

#endif // AIWORLD_DYNAMICQUESTKILLEVENT_H
