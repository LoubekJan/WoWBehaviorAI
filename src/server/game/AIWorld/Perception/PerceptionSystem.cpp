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

    observation.Type = ObservationType::WorldEvent;

    observation.SourceEventId = event.EventId;
    observation.CorrelationId = event.CorrelationId;
    observation.ObservedAtMs = event.OccurredAtMs;
    observation.SourceEventType = event.Type;

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
    observation.Type = ObservationType::PlayerSeen;

    // No underlying WorldEvent: this is a periodic nearby-entity scan, not
    // a reaction to something that happened. SourceEventId/CorrelationId
    // stay 0, SourceEventType stays nullopt, and ObservedAtMs is stamped
    // to now rather than copied from an OccurredAtMs that doesn't exist
    // here.
    observation.SourceEventId = 0;
    observation.CorrelationId = 0;
    observation.ObservedAtMs = uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
        GameTime::GetSystemTime().time_since_epoch()).count());

    observation.Location.MapId = player.GetMapId();
    observation.Location.X = player.GetPositionX();
    observation.Location.Y = player.GetPositionY();
    observation.Location.Z = player.GetPositionZ();

    // Target, not Actor: consistent with CreatureKilled's Actor=did-it/
    // Target=had-it-done-to-them split, the seen player is the one being
    // perceived, not the one performing an action. Every future unary
    // observation (nothing "done", just "there") should follow the same
    // rule, so Memory never needs type-specific cases to answer "who did I
    // see?". Actor left unset.
    observation.Target.Guid = player.GetGUID();
    observation.Target.Entry = player.GetEntry();

    observation.Channel = PerceptionChannel::Sight;
    observation.Distance = distance;
    observation.LineOfSight = true;

    return observation;
}

std::optional<Observation> PerceptionSystem::ObserveNearbyCreature(AgentId observerId, Creature const& observer,
    Creature const& seen, float sightRange) const
{
    if (!observer.IsAlive())
        return std::nullopt;

    if (!seen.IsAlive())
        return std::nullopt;

    if (&observer == &seen)
        return std::nullopt;

    if (observer.GetMapId() != seen.GetMapId())
        return std::nullopt;

    float distance = observer.GetDistance(seen.GetPositionX(), seen.GetPositionY(), seen.GetPositionZ());
    if (distance > sightRange)
        return std::nullopt;

    if (!observer.IsWithinLOS(seen.GetPositionX(), seen.GetPositionY(), seen.GetPositionZ()))
        return std::nullopt;

    Observation observation;
    observation.Observer = observerId;
    observation.Type = ObservationType::CreatureSeen;

    // No underlying WorldEvent, same as ObserveNearbyPlayer.
    observation.SourceEventId = 0;
    observation.CorrelationId = 0;
    observation.ObservedAtMs = uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
        GameTime::GetSystemTime().time_since_epoch()).count());

    observation.Location.MapId = seen.GetMapId();
    observation.Location.X = seen.GetPositionX();
    observation.Location.Y = seen.GetPositionY();
    observation.Location.Z = seen.GetPositionZ();

    // Target, not Actor - same convention as ObserveNearbyPlayer. SpawnId
    // is what lets AIWorldMgr enrich this with the seen creature's AgentId
    // afterward, if it has one; PerceptionSystem itself never touches
    // AgentRegistry.
    observation.Target.Guid = seen.GetGUID();
    observation.Target.Entry = seen.GetEntry();
    observation.Target.SpawnId = seen.GetSpawnId();

    observation.Channel = PerceptionChannel::Sight;
    observation.Distance = distance;
    observation.LineOfSight = true;

    return observation;
}
