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
#include "GameTime.h"
#include "Player.h"
#include <chrono>

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

std::optional<Observation> PerceptionSystem::ObserveNearbyPlayer(AgentId observerId, Creature const& observer,
    Player const& player, float sightRange) const
{
    if (!observer.IsAlive())
        return std::nullopt;

    if (observer.GetMapId() != player.GetMapId())
        return std::nullopt;

    float distance = observer.GetDistance(player.GetPositionX(), player.GetPositionY(), player.GetPositionZ());
    if (distance > sightRange)
        return std::nullopt;

    if (!observer.IsWithinLOS(player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()))
        return std::nullopt;

    Observation observation;
    observation.Observer = observerId;

    // No underlying WorldEvent: this is a periodic nearby-entity scan, not
    // a reaction to something that happened. SourceEventId/CorrelationId
    // stay 0, and ObservedAtMs is stamped to now rather than copied from
    // an OccurredAtMs that doesn't exist here.
    observation.SourceEventId = 0;
    observation.CorrelationId = 0;
    observation.ObservedAtMs = uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
        GameTime::GetSystemTime().time_since_epoch()).count());

    observation.EventType = WorldEventType::PlayerSeen;

    observation.Location.MapId = player.GetMapId();
    observation.Location.X = player.GetPositionX();
    observation.Location.Y = player.GetPositionY();
    observation.Location.Z = player.GetPositionZ();

    observation.Actor.Guid = player.GetGUID();
    observation.Actor.Entry = player.GetEntry();
    // Target left unset: a PlayerSeen observation has one subject (the
    // player seen), not an actor/target pair.

    observation.Channel = PerceptionChannel::Sight;
    observation.Distance = distance;
    observation.LineOfSight = true;

    return observation;
}
