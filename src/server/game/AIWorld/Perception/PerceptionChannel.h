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

#ifndef AIWORLD_PERCEPTIONCHANNEL_H
#define AIWORLD_PERCEPTIONCHANNEL_H

#include "Define.h"

// Sight is the physical perception channel (range + LOS against a
// witnessed WorldEvent or nearby entity - PerceptionSystem::ObserveEvent()/
// ObserveNearbyPlayer()/ObserveNearbyCreature()). Milestone 2.13C6C gave
// Rumor its first real logic: targeted directed-event delivery for
// dynamic quest outcomes (PerceptionSystem::ObserveDirectedEvent()),
// value-only and independent of range/LOS/materialization. Hearing is
// still not implemented.
enum class PerceptionChannel : uint8
{
    Sight,
    Hearing,
    Rumor
};

inline char const* ToString(PerceptionChannel channel)
{
    switch (channel)
    {
        case PerceptionChannel::Sight:   return "SIGHT";
        case PerceptionChannel::Hearing: return "HEARING";
        case PerceptionChannel::Rumor:   return "RUMOR";
        default:                         return "UNKNOWN";
    }
}

#endif // AIWORLD_PERCEPTIONCHANNEL_H
