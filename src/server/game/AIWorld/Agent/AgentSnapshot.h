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

#ifndef AIWORLD_AGENTSNAPSHOT_H
#define AIWORLD_AGENTSNAPSHOT_H

#include "Define.h"
#include "ObjectGuid.h"

// Read-only, POD view of a tracked Creature at a point in time.
// Never holds a pointer back into the Map/Creature/Player object graph -
// AIWorld code must not touch those outside the world update thread.
struct AgentSnapshot
{
    uint64 SpawnId = 0;
    ObjectGuid Guid;
    uint32 Entry = 0;

    uint32 MapId = 0;

    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Orientation = 0.0f;

    uint32 Health = 0;
    uint32 MaxHealth = 0;

    bool Alive = false;
    bool InCombat = false;

    uint64 SnapshotSequence = 0;
};

#endif // AIWORLD_AGENTSNAPSHOT_H
