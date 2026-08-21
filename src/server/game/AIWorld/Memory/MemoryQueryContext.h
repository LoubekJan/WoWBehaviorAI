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

#ifndef AIWORLD_MEMORYQUERYCONTEXT_H
#define AIWORLD_MEMORYQUERYCONTEXT_H

#include "Agent/AgentId.h"
#include "Define.h"

// What "now, for this agent, standing here" means for retrieval scoring -
// built straight from an AgentSnapshot (already a pure value snapshot
// with AgentId, map, and position - no live TC pointers), not from a
// Creature directly.
struct MemoryQueryContext
{
    AgentId Agent;

    uint64 NowMs = 0;

    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_MEMORYQUERYCONTEXT_H
