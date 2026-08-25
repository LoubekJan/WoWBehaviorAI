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

#ifndef AIWORLD_CREATUREGROUPMEMBER_H
#define AIWORLD_CREATUREGROUPMEMBER_H

#include "Define.h"

// Milestone 2.12C: persistent membership - which real TrinityCore creature
// spawn belongs to a CreatureGroup, nothing more. Loaded once at startup
// (AgentPersistence::LoadCreatureGroupMembers()) from
// ai_creature_group_members and never mutated at runtime in this
// milestone - no spawn/despawn from AI, no membership change. Deliberately
// NOT an ObjectGuid/RuntimeGuid: a spawn identifies the same creature.guid
// row across every load/unload cycle, the same stable-identity reasoning
// AgentRecord::SpawnId already relies on for an individual agent. Pure
// value: no Creature*/Map*.
struct CreatureGroupMember
{
    uint32 MapId = 0;
    uint64 SpawnId = 0;
};

#endif // AIWORLD_CREATUREGROUPMEMBER_H
