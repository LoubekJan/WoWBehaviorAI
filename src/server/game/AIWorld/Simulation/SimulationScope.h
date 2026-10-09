/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_SIMULATION_SCOPE_H
#define AIWORLD_SIMULATION_SCOPE_H

#include "Define.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

struct SimulationBounds
{
    float MinX = 0, MaxX = 0, MinY = 0, MaxY = 0;

    bool Valid() const
    {
        return std::isfinite(MinX) && std::isfinite(MaxX) && std::isfinite(MinY) &&
            std::isfinite(MaxY) && MinX < MaxX && MinY < MaxY;
    }
};

// Value-only startup configuration. ZoneIds are root zones (GetZoneId), so
// the default also includes Goldshire and other Elwynn subareas.
// A home belongs to an individual spawn; it is never replaced by this scope.
struct SimulationScope
{
    uint32 MapId = 0;
    std::vector<uint32> ZoneIds{12};
    std::optional<SimulationBounds> Bounds;
    std::vector<uint64> SpawnIds;

    bool Valid() const
    {
        if (ZoneIds.empty() || ZoneIds.size() > 64 || SpawnIds.size() > 4096 ||
            (Bounds && !Bounds->Valid()))
            return false;
        for (auto i = ZoneIds.begin(); i != ZoneIds.end(); ++i)
            if (!*i || std::find(ZoneIds.begin(), i, *i) != i)
                return false;
        for (auto i = SpawnIds.begin(); i != SpawnIds.end(); ++i)
            if (!*i || std::find(SpawnIds.begin(), i, *i) != i)
                return false;
        return true;
    }

    bool ContainsMapZone(uint32 map, uint32 zone) const
    {
        return Valid() && map == MapId && std::find(ZoneIds.begin(), ZoneIds.end(), zone) != ZoneIds.end();
    }

    bool ContainsPosition(uint32 map, float x, float y, float z) const
    {
        return Valid() && map == MapId && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
            (!Bounds || (x >= Bounds->MinX && x <= Bounds->MaxX && y >= Bounds->MinY && y <= Bounds->MaxY));
    }

    bool Contains(uint32 map, uint32 zone, float x, float y, float z) const
    {
        return ContainsMapZone(map, zone) && ContainsPosition(map, x, y, z);
    }

    bool ContainsSpawn(uint64 spawn) const
    {
        return Valid() && (SpawnIds.empty() || std::find(SpawnIds.begin(), SpawnIds.end(), spawn) != SpawnIds.end());
    }

    // Static membership. An owned NPC that leaves the bounds stays observable;
    // live position validation belongs to movement/perception, not ownership.
    bool ContainsActor(uint32 map, uint64 spawn) const
    {
        return map == MapId && ContainsSpawn(spawn);
    }
};

#endif
