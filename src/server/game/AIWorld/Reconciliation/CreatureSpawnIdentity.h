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

#ifndef AIWORLD_CREATURESPAWNIDENTITY_H
#define AIWORLD_CREATURESPAWNIDENTITY_H

#include "Define.h"

// Milestone 2.12F4B: one eligible persistent non-instance world.creature
// spawn, as a pure value - see CreatureSpawnCensus.h for how this is built
// and what "eligible" means. No Creature*/Map*/Unit* - Entry comes
// straight from ObjectMgr's own already-loaded CreatureData spawn row
// (creature_template entry); NpcFlags is the EFFECTIVE npcflag (2.12F4B
// P2 fix, STATIC review) - creature.npcflag with creature_template's own
// npcflag as fallback when the spawn has no override (0), the same
// ObjectMgr::ChooseCreatureFlags() semantics TrinityCore itself applies
// at creature load time - never the raw per-spawn override column alone.
struct CreatureSpawnIdentity
{
    uint32 MapId = 0;
    uint64 SpawnId = 0;
    uint32 Entry = 0;
    uint32 NpcFlags = 0;
};

#endif // AIWORLD_CREATURESPAWNIDENTITY_H
