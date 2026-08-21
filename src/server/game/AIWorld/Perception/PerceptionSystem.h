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

// Turns an objective fact - a WorldEvent, or (Milestone 2.4B) simply a
// Player currently near an agent - into a subjective Observation for one
// specific agent, or nothing if that agent couldn't actually have
// perceived it. Deliberately takes a live Creature&/Player const&, so it
// must only ever be called from the world thread, after AIWorldMgr has
// already resolved the observer's (and, for ObserveNearbyPlayer, the seen
// player's) live object - never from a map/combat worker, never from
// anything that only has a value-only AgentRecord.
class TC_GAME_API PerceptionSystem
{
    public:
        // nullopt if: observer is dead, observer isn't on the event's map,
        // the event is out of sightRange, or there's no line of sight to
        // its location. Only PerceptionChannel::Sight is implemented -
        // Hearing/Rumor have no logic yet.
        std::optional<Observation> ObserveEvent(AgentId observerId, Creature const& observer,
            WorldEvent const& event, float sightRange) const;

        // Same gating as ObserveEvent (dead/map/range/LOS), but against a
        // Player's current position rather than a WorldEvent's recorded
        // one, and with no underlying WorldEvent to inherit identity from:
        // the resulting Observation always has SourceEventId=0,
        // CorrelationId=0, EventType=PlayerSeen, Target=the seen player
        // (Actor left unset - consistent with CreatureKilled's Actor=
        // did-it/Target=had-it-done-to-them split: the seen player isn't
        // performing an action, just being perceived), and ObservedAtMs
        // stamped to the current wall-clock time (there is no OccurredAtMs
        // to copy).
        std::optional<Observation> ObserveNearbyPlayer(AgentId observerId, Creature const& observer,
            Player const& player, float sightRange) const;
};

#endif // AIWORLD_PERCEPTIONSYSTEM_H
