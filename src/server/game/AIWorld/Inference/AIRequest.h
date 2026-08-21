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

#ifndef AIWORLD_AIREQUEST_H
#define AIWORLD_AIREQUEST_H

#include "Agent/AgentId.h"
#include "Define.h"

enum class AIRequestType : uint8
{
    Health = 0,
    Decision = 1
};

// Plain data handed to AIClient. Never carries a Creature*/Player*/Map* -
// AIClient and everything below it only ever sees values, never live
// game objects. Agent and the Health/Entry/.../Z fields mirror AgentSnapshot
// and are only meaningful (and only sent) for Type == Decision.
struct AIRequest
{
    uint64 RequestId = 0;
    AIRequestType Type = AIRequestType::Health;
    AgentId Agent;
    uint64 SnapshotSequence = 0;
    uint64 SpawnId = 0;

    uint32 Entry = 0;
    uint32 Health = 0;
    uint32 MaxHealth = 0;
    bool Alive = false;
    bool InCombat = false;

    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_AIREQUEST_H
