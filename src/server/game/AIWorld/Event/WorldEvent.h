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

#ifndef AIWORLD_WORLDEVENT_H
#define AIWORLD_WORLDEVENT_H

#include "Define.h"
#include "WorldEntityRef.h"
#include "WorldEventType.h"

// A transient runtime fact - not the later, separate persisted/audit form
// (HistoricalEvent). Pure value object: no Creature*/Player*/Unit*/Map*
// anywhere, so it can safely cross from whatever thread observed it (a
// map/combat worker) to the world thread through EventBus without any of
// those pointers' lifetimes mattering.
//
// EventId and CorrelationId are assigned by EventBus::Publish(), not by
// whoever constructs the event - leave both at 0 to publish a root event
// (Publish() sets CorrelationId = EventId in that case). A caused-by event
// should set CauseEventId to the causing event's EventId and CorrelationId
// to that event's own CorrelationId (inheriting the root cause's
// correlation, not starting a new one).
struct WorldEvent
{
    uint64 EventId = 0;
    uint64 CorrelationId = 0;
    uint64 CauseEventId = 0;

    uint64 OccurredAtMs = 0;

    WorldEventType Type = WorldEventType::CreatureKilled;

    WorldEventLocation Location;
    WorldEntityRef Actor;
    WorldEntityRef Target;
};

#endif // AIWORLD_WORLDEVENT_H
