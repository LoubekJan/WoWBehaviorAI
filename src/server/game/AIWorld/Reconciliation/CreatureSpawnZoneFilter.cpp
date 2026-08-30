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

#include "CreatureSpawnZoneFilter.h"
#include "DatabaseEnv.h"

std::unordered_set<uint64> FetchCreatureSpawnIdsForZone(uint32 zoneId)
{
    std::unordered_set<uint64> spawnIds;

    WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_SEL_CREATURE_GUIDS_BY_ZONE);
    stmt->setUInt32(0, zoneId);
    PreparedQueryResult result = WorldDatabase.Query(stmt);
    if (!result)
        return spawnIds;

    spawnIds.reserve(result->GetRowCount());
    do
    {
        Field* fields = result->Fetch();
        spawnIds.insert(fields[0].GetUInt32());
    } while (result->NextRow());

    return spawnIds;
}
