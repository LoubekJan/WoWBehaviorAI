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

// Turns an objective WorldEvent into a subjective Observation for one
// specific agent - or nothing, if that agent couldn't actually have
// witnessed it. Deliberately takes a live Creature&, so it must only ever
// be called from the world thread, after AIWorldMgr has already resolved
// the observer's Creature (never from a map/combat worker, never from
// anything that only has a value-only AgentRecord).
class TC_GAME_API PerceptionSystem
{
    public:
        // nullopt if: observer is dead, observer isn't on the event's map,
        // the event is out of sightRange, or there's no line of sight to
        // its location. Only PerceptionChannel::Sight is implemented -
        // Hearing/Rumor have no logic yet.
        std::optional<Observation> ObserveEvent(AgentId observerId, Creature const& observer,
            WorldEvent const& event, float sightRange) const;
};

#endif // AIWORLD_PERCEPTIONSYSTEM_H
