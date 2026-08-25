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

#ifndef AIWORLD_CREATUREGROUPRUNTIMEVIEW_H
#define AIWORLD_CREATUREGROUPRUNTIMEVIEW_H

#include "Define.h"

// Milestone 2.12C: a transient, per-tick snapshot of how many of a
// CreatureGroup's persistent members are naturally present right now -
// built fresh every coarse tick from a live (but never stored)
// Map::GetCreatureBySpawnId() lookup per CreatureGroupMember, never cached
// on AgentRecord itself. LoadedMembers > 0 is the whole invariant this
// milestone acts on: real TrinityCore wolves become the authority and
// AIWorldMgr::RunDecisionScheduler() stops calling
// CreatureGroupSimulationSystem::Update() for as long as it holds -
// AliveLoadedMembers is tracked/logged for later roadmap work
// (population loss from death, ...), not acted on by this milestone.
// Pure value: no Creature*/Map*/ObjectGuid.
struct CreatureGroupRuntimeView
{
    uint32 TotalMembers = 0;
    uint32 LoadedMembers = 0;
    uint32 AliveLoadedMembers = 0;
};

#endif // AIWORLD_CREATUREGROUPRUNTIMEVIEW_H
