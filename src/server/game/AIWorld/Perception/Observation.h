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
#include "PerceptionChannel.h"

// What a specific agent actually perceived out of an objective WorldEvent -
// never the event itself. A WorldEvent is ground truth about the world; an
// Observation only exists for the one Observer that was actually in range
// (and, for Sight, had line of sight) when it happened. Pure value object,
// same rule as WorldEvent/AgentRecord: no Creature*/Player*/Map* anywhere,
// so it can safely flow on into Memory/decision context later.
struct Observation
{
    AgentId Observer;

    uint64 SourceEventId = 0;
    uint64 CorrelationId = 0;
    uint64 ObservedAtMs = 0;

    WorldEventType EventType = WorldEventType::CreatureKilled;

    WorldEventLocation Location;
    WorldEntityRef Actor;
    WorldEntityRef Target;

    PerceptionChannel Channel = PerceptionChannel::Sight;

    float Distance = 0.0f;
    bool LineOfSight = false;
};

#endif // AIWORLD_OBSERVATION_H
