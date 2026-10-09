/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef MMAP_DETAIL_MESH_BOUNDS_H
#define MMAP_DETAIL_MESH_BOUNDS_H

#include "DetourNavMeshBuilder.h"
#include <algorithm>
#include <cmath>

namespace MMAP
{
    inline void IncludeDetailMeshHeightBounds(dtNavMeshCreateParams& params, bool includeFlatTerrain)
    {
        if (!includeFlatTerrain || !params.detailVerts)
            return;

        // Recast can lift a constant floor while voxelizing it. Detour builds
        // BV nodes from these detail vertices and clamps queries to bmax.
        // Keep bmin unchanged: it is also the origin of quantized poly verts.
        for (int i = 0; i < params.detailVertsCount; ++i)
        {
            float const height = params.detailVerts[i * 3 + 1];
            if (std::isfinite(height))
                params.bmax[1] = std::max(params.bmax[1], height);
        }
    }
}

#endif
