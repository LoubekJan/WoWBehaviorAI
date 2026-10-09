/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_SIMULATION_PATH_BOUNDS_H
#define AIWORLD_SIMULATION_PATH_BOUNDS_H

#include "MovementPathBounds.h"
#include "SimulationScope.h"

namespace Movement
{
    // Prove the whole corridor, including root-zone transitions between nav
    // corners. Invalid scopes cannot silently widen a scoped movement.
    template<class Path, class ZoneAt>
    bool PathWithinSimulationScope(Path const& path, uint32 mapId, SimulationScope const& scope, ZoneAt&& zoneAt)
    {
        if (!scope.Valid() || mapId != scope.MapId)
            return false;
        return PathWithinBounds(path, [&](float x, float y, float z)
        {
            return scope.ContainsPosition(mapId, x, y, z) && scope.ContainsMapZone(mapId, zoneAt(x, y, z));
        });
    }
}
#endif
