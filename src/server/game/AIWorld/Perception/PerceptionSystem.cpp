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

#include "PerceptionSystem.h"
#include "Creature.h"

std::optional<Observation> PerceptionSystem::ObserveEvent(AgentId observerId, Creature const& observer,
    WorldEvent const& event, float sightRange) const
{
    if (!observer.IsAlive())
        return std::nullopt;

    if (observer.GetMapId() != event.Location.MapId)
        return std::nullopt;

    float distance = observer.GetDistance(event.Location.X, event.Location.Y, event.Location.Z);
    if (distance > sightRange)
        return std::nullopt;

    if (!observer.IsWithinLOS(event.Location.X, event.Location.Y, event.Location.Z))
        return std::nullopt;

    Observation observation;
    observation.Observer = observerId;

    observation.SourceEventId = event.EventId;
    observation.CorrelationId = event.CorrelationId;
    observation.ObservedAtMs = event.OccurredAtMs;

    observation.EventType = event.Type;

    observation.Location = event.Location;
    observation.Actor = event.Actor;
    observation.Target = event.Target;

    observation.Channel = PerceptionChannel::Sight;
    observation.Distance = distance;
    observation.LineOfSight = true;

    return observation;
}
