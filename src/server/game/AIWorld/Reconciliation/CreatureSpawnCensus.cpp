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

#include "CreatureSpawnCensus.h"
#include "DBCStores.h"
#include "ObjectMgr.h"

std::vector<CreatureSpawnIdentity> BuildCreatureSpawnCensus()
{
    std::vector<CreatureSpawnIdentity> census;

    CreatureDataContainer const& spawns = sObjectMgr->GetAllCreatureData();
    census.reserve(spawns.size());

    for (auto const& entry : spawns)
    {
        CreatureData const& data = entry.second;

        // 2.12F4A2/2.12F4B Scope predicate - deterministic/static, no live
        // Map* required: MapEntry exists AND Instanceable() == false. A
        // spawn on a nonexistent map never reaches _creatureDataStore in
        // the first place (ObjectMgr::LoadCreatures() itself already
        // skips/logs that case), so only the Instanceable() check is
        // actually needed here.
        MapEntry const* mapEntry = sMapStore.LookupEntry(data.mapId);
        if (!mapEntry || mapEntry->Instanceable())
            continue;

        CreatureSpawnIdentity identity;
        identity.MapId = data.mapId;
        identity.SpawnId = data.spawnId;
        identity.Entry = data.id;
        identity.NpcFlags = data.npcflag;
        census.push_back(identity);
    }

    return census;
}
