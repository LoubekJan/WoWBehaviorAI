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

#ifndef AIWORLD_OBSERVATIONTYPE_H
#define AIWORLD_OBSERVATIONTYPE_H

#include "Define.h"

// What kind of perception produced an Observation - deliberately a
// separate enum from WorldEventType, not a reuse of it. WorldEventType is
// objective ground truth about the world (what actually happened);
// ObservationType is about how an agent came to perceive something, which
// isn't always "a WorldEvent occurred" - PlayerSeen/CreatureSeen come from
// a periodic nearby-entity scan with no underlying WorldEvent at all.
// Keeping them separate means a future consumer (Memory) never has to
// guess whether a given Observation's type names a real world event or
// just a sensor-sample kind.
enum class ObservationType : uint8
{
    WorldEvent,
    PlayerSeen,
    CreatureSeen
};

inline char const* ToString(ObservationType type)
{
    switch (type)
    {
        case ObservationType::WorldEvent:   return "WORLD_EVENT";
        case ObservationType::PlayerSeen:   return "PLAYER_SEEN";
        case ObservationType::CreatureSeen: return "CREATURE_SEEN";
        default:                            return "UNKNOWN";
    }
}

#endif // AIWORLD_OBSERVATIONTYPE_H
