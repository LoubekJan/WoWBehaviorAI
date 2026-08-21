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

// Milestone 2.4A only ever produces Sight observations (range + LOS
// against a witnessed WorldEvent). Hearing and Rumor are declared now,
// per the roadmap, so Observation doesn't need to change shape once a
// later milestone actually implements them - neither has any logic yet.
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
