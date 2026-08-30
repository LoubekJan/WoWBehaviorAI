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

#ifndef AIWORLD_CREATURESPAWNZONEFILTER_H
#define AIWORLD_CREATURESPAWNZONEFILTER_H

#include "Define.h"
#include <unordered_set>

// Milestone 2.12F4B2: a narrow, one-shot WorldDatabase read of every
// creature.guid whose stored zoneId matches the given value - deliberately
// kept out of CreatureSpawnCensus.h/.cpp (that module's own doc comment
// promises "no SQL query of its own"; this is the one place in the
// reconciliation pipeline that touches WorldDatabase). Reads
// creature.zoneId as-is, never computes zone/area itself -
// Map::GetZoneAndAreaId() would require a live Map* (sMapMgr->
// CreateBaseMap()) and terrain/vmap data from disk, which this AIWorld
// subsystem's census pipeline has deliberately never depended on (see
// CreatureSpawnCensus.h's own comment). See AIWorld_Current_Roadmap.md's
// own "2.12F4B2" section for the precondition this depends on:
// creature.zoneId must be freshly recalculated (Calculate.Creature.Zone.
// Area.Data run once) for the CURRENT world.creature state - a stale or
// never-recalculated row simply reads as zoneId = 0 and is excluded, not
// mistaken for a real match.
TC_GAME_API std::unordered_set<uint64> FetchCreatureSpawnIdsForZone(uint32 zoneId);

#endif // AIWORLD_CREATURESPAWNZONEFILTER_H
