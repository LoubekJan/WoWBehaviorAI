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

#ifndef AIWORLD_PERCEPTIONSYSTEM_H
#define AIWORLD_PERCEPTIONSYSTEM_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "Event/WorldEvent.h"
#include "Observation.h"
#include <optional>

class Creature;
class Player;

// Turns an objective fact - a WorldEvent, or (Milestone 2.4B/2.4C) simply a
// Player/Creature currently near an agent - into a subjective Observation
// for one specific agent, or nothing if that agent couldn't actually have
// perceived it.
//
// Sight observations (ObserveEvent()/ObserveNearbyPlayer()/
// ObserveNearbyCreature()) are physical: they deliberately take live
// references and require a live observer, range, and LOS, so they must
// only ever be called from the world thread, after AIWorldMgr has already
// resolved the observer's (and, for the ObserveNearby* methods, the seen
// entity's) live object - never from a map/combat worker, never from
// anything that only has a value-only AgentRecord.
//
// Milestone 2.13C6C: directed Rumor observations (ObserveDirectedEvent())
// are value-only targeted delivery instead - no live observer, range, or
// LOS required, since the underlying event explicitly names the
// recipient. Hearing remains unimplemented.
//
// Never touches AgentRegistry itself - AgentId enrichment of a returned
// Observation's Target/Actor is AIWorldMgr's job, not this class's.
class TC_GAME_API PerceptionSystem
{
    public:
        // nullopt if: observer is dead, observer isn't on the event's map,
        // the event is out of sightRange, or there's no line of sight to
        // its location. Physical Sight perception only - see
        // ObserveDirectedEvent() below for the directed Rumor path.
        std::optional<Observation> ObserveEvent(AgentId observerId, Creature const& observer,
            WorldEvent const& event, float sightRange) const;

        // Same gating as ObserveEvent (dead/map/range/LOS), but against a
        // Player's current position rather than a WorldEvent's recorded
        // one, and with no underlying WorldEvent to inherit identity from:
        // the resulting Observation always has SourceEventId=0,
        // CorrelationId=0, SourceEventType=nullopt, Type=PlayerSeen,
        // Target=the seen player (Actor left unset - consistent with
        // CreatureKilled's Actor=did-it/Target=had-it-done-to-them split:
        // the seen player isn't performing an action, just being
        // perceived), and ObservedAtMs stamped to the current wall-clock
        // time (there is no OccurredAtMs to copy).
        std::optional<Observation> ObserveNearbyPlayer(AgentId observerId, Creature const& observer,
            Player const& player, float sightRange) const;

        // Same shape as ObserveNearbyPlayer but for a nearby Creature:
        // Type=CreatureSeen, Target=the seen creature (Actor unset, same
        // reasoning). Also rejects observer == seen (a Creature never
        // "sees" itself) in addition to the usual dead/map/range/LOS
        // checks - both observer and seen must be alive.
        std::optional<Observation> ObserveNearbyCreature(AgentId observerId, Creature const& observer,
            Creature const& seen, float sightRange) const;

        // Milestone 2.13C6C: the Rumor channel's first real logic - a
        // targeted fallback for an agent this WorldEvent explicitly names
        // (event.Target.Agent), used when that agent could not actually
        // witness the event through normal Sight (e.g. a dynamic quest
        // issuer that is unloaded/dead/out of range when its own quest
        // resolves). Deliberately takes no Creature* and applies no map/
        // range/LOS gate whatsoever - unlike ObserveEvent() above, this is
        // never "did the observer perceive this happening nearby", only
        // "this event is explicitly about this specific agent, and that is
        // reason enough for it to know". Requires event.Target.Agent ==
        // observerId exactly (nullopt otherwise, including for an empty/
        // default observerId) - this must never be used to hand an agent
        // an observation of an event that does not actually name it.
        // Copies EventId/CorrelationId/OccurredAtMs/Type/Location/Actor/
        // Target straight from the event, same as ObserveEvent(); Channel
        // is always PerceptionChannel::Rumor, Distance is always 0.0f, and
        // LineOfSight is always false - there is no real distance/LOS
        // concept for a directed, non-physical delivery.
        std::optional<Observation> ObserveDirectedEvent(AgentId observerId, WorldEvent const& event) const;
};

#endif // AIWORLD_PERCEPTIONSYSTEM_H
