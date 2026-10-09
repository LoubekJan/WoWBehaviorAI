#ifndef TRINITY_ALWAYS_ACTIVE_ZONE_H
#define TRINITY_ALWAYS_ACTIVE_ZONE_H

#include "GridDefines.h"
#include <cmath>
#include <vector>

// Conservative coverage of a zone census: include empty interior grids and a
// one-grid movement buffer. The zoneId predicate belongs to the DB caller.
class AlwaysActiveZoneCoverage
{
public:
    bool AddSpawn(float x, float y)
    {
        if (!Trinity::IsValidMapCoord(x, y))
            return false;
        GridCoord grid = Trinity::ComputeGridCoord(x, y);
        if (!grid.IsCoordValid())
            return false;
        _low.x_coord = std::min(_low.x_coord, grid.x_coord);
        _low.y_coord = std::min(_low.y_coord, grid.y_coord);
        _high.x_coord = std::max(_high.x_coord, grid.x_coord);
        _high.y_coord = std::max(_high.y_coord, grid.y_coord);
        _hasSpawns = true;
        return true;
    }

    bool AddBounds(float minX, float maxX, float minY, float maxY)
    {
        if (!std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) || !std::isfinite(maxY) ||
            minX >= maxX || minY >= maxY || !Trinity::IsValidMapCoord(minX, minY) ||
            !Trinity::IsValidMapCoord(maxX, maxY))
            return false;
        return AddSpawn(minX, minY) && AddSpawn(maxX, maxY);
    }

    std::vector<GridCoord> GetGrids(uint32 padding = 1) const
    {
        if (!_hasSpawns)
            return {};
        GridCoord low = _low, high = _high;
        low.dec_x(padding);
        low.dec_y(padding);
        high.inc_x(padding);
        high.inc_y(padding);
        std::vector<GridCoord> grids;
        for (uint32 x = low.x_coord; x <= high.x_coord; ++x)
            for (uint32 y = low.y_coord; y <= high.y_coord; ++y)
                grids.emplace_back(x, y);
        return grids;
    }

private:
    GridCoord _low{MAX_NUMBER_OF_GRIDS - 1, MAX_NUMBER_OF_GRIDS - 1};
    GridCoord _high{0, 0};
    bool _hasSpawns = false;
};

#endif
