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

#ifndef AIWORLD_AGENTLOCATION_H
#define AIWORLD_AGENTLOCATION_H

#include "Define.h"

// Milestone 2.11A: a pure, persisted world point - no Creature*/Map*, no
// behavior. Used for AgentRecord::HomeLocation/WorkLocation, the only two
// uses so far. Deliberately not the same type as anything MOVE_TO/Action
// might later resolve a destination through - this is durable data the AI
// pipeline does not yet act on, not a goal target.
struct AgentLocation
{
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Orientation = 0.0f;
};

#endif // AIWORLD_AGENTLOCATION_H
