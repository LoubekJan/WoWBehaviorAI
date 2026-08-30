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

        // 2.12F4B P2 fix (STATIC review): data.npcflag is the per-spawn
        // OVERRIDE column, not the effective flags - creature.npcflag = 0
        // conventionally means "no override, use creature_template's own
        // value", the exact fallback ObjectMgr::ChooseCreatureFlags()
        // itself applies at creature load time (Creature::UpdateEntry()).
        // Reading data.npcflag directly would misclassify most real
        // vendors (creature.npcflag = 0, creature_template.npcflag =
        // VENDOR) as Unclassified. cInfo should never be null here -
        // ObjectMgr::LoadCreatures() already skips/rejects any creature
        // row with a nonexistent template before it ever reaches
        // _creatureDataStore - but this falls back to the raw override
        // rather than dereferencing a null CreatureTemplate if it somehow
        // is.
        uint32 effectiveNpcFlags = data.npcflag;
        if (CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id))
            ObjectMgr::ChooseCreatureFlags(cInfo, &effectiveNpcFlags, nullptr, nullptr, &data);
        identity.NpcFlags = effectiveNpcFlags;

        census.push_back(identity);
    }

    return census;
}

std::unordered_set<uint64> BuildAllKnownCreatureSpawnIds()
{
    CreatureDataContainer const& spawns = sObjectMgr->GetAllCreatureData();

    std::unordered_set<uint64> spawnIds;
    spawnIds.reserve(spawns.size());
    for (auto const& entry : spawns)
        spawnIds.insert(entry.second.spawnId);

    return spawnIds;
}
