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

#ifndef AIWORLD_WORLDENTITYREF_H
#define AIWORLD_WORLDENTITYREF_H

#include "Agent/AgentId.h"
#include "Define.h"
#include "ObjectGuid.h"

struct WorldEventLocation
{
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// Value-only reference to whatever took part in a WorldEvent - never a
// Creature*/Player*/Unit*. Whoever publishes the event only ever fills in
// Guid/SpawnId/Entry (all read straight off the live object, no registry
// lookups). Agent stays AgentId{} (0) until AIWorldMgr::ProcessWorldEvent()
// enriches it via an AgentRegistry lookup on the world thread - registry
// lookups never happen on whatever thread published the event.
struct WorldEntityRef
{
    ObjectGuid Guid = ObjectGuid::Empty;

    uint64 SpawnId = 0;
    uint32 Entry = 0;

    AgentId Agent;
};

#endif // AIWORLD_WORLDENTITYREF_H
