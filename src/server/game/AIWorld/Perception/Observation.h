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

#ifndef AIWORLD_OBSERVATION_H
#define AIWORLD_OBSERVATION_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEntityRef.h"
#include "Event/WorldEventType.h"
#include "ObservationType.h"
#include "PerceptionChannel.h"
#include <optional>

// What a specific agent actually perceived - never the underlying fact
// itself, if there even is one. An Observation only exists for the one
// Observer that was actually in range (and, for Sight, had line of sight)
// at the time. Pure value object, same rule as WorldEvent/AgentRecord: no
// Creature*/Player*/Map* anywhere, so it can safely flow on into Memory/
// decision context later.
//
// Type says what kind of perception this is; SourceEventType is only ever
// set (to the causing WorldEvent's Type) when Type == ObservationType::
// WorldEvent. PlayerSeen/CreatureSeen observations - from a periodic
// nearby-entity scan, not a witnessed WorldEvent - leave it nullopt and
// SourceEventId/CorrelationId/SourceOccurredAtMs at 0.
//
// SourceOccurredAtMs and ObservedAtMs are deliberately not the same field:
// the former is when the underlying fact happened (copied from a
// WorldEvent's OccurredAtMs, 0 if there isn't one), the latter is when
// this specific agent actually perceived it (always real wall-clock time
// this Observation was built, never copied from anything). Memory's TTL
// has to be anchored to ObservedAtMs - a WorldEvent that already sat in
// the queue for a while before being witnessed must not get a head start
// toward expiry it didn't earn.
struct Observation
{
    AgentId Observer;

    ObservationType Type = ObservationType::WorldEvent;

    uint64 SourceEventId = 0;
    uint64 CorrelationId = 0;
    uint64 SourceOccurredAtMs = 0;
    uint64 ObservedAtMs = 0;

    std::optional<WorldEventType> SourceEventType;

    WorldEventLocation Location;
    WorldEntityRef Actor;
    WorldEntityRef Target;

    PerceptionChannel Channel = PerceptionChannel::Sight;

    float Distance = 0.0f;
    bool LineOfSight = false;
};

#endif // AIWORLD_OBSERVATION_H
