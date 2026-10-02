/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_COMPLETECHASEPATH_H
#define TRINITY_COMPLETECHASEPATH_H

#include "MovementPathBounds.h"
#include "PathType.h"
#include <utility>

namespace Movement
{
    // Used both before starting a hunt and for every chase refresh. The
    // shortened path is the actual spline, so validate it again after trimming.
    template<class Generator, class Point, class Points, class Contains, class ClearSegment>
    bool PrepareCompleteChasePath(Generator& path, Point const& origin, Point const& destination,
        Point const& target, float stopDistance, bool shorten, Points& output,
        Contains&& contains, ClearSegment&& clearSegment)
    {
        output.clear();
        path.AllowSteepSlopes();
        if (!path.CalculatePath(destination.x, destination.y, destination.z, false) ||
            !CompleteNavmeshPath(path.GetPathType()) || !PathWithinBounds(path.GetPath(), contains)) return false;
        if (shorten) path.ShortenPathUntilDist(target, stopDistance);
        auto points = path.GetPath();
        if (points.size() < 2 || !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
            std::hypot(points.front().x-origin.x, points.front().y-origin.y, points.front().z-origin.z) > 1.5f) return false;
        points.front() = origin;
        if (!PathWithinBounds(points, contains) ||
            (shorten && !clearSegment(points[points.size()-2], points.back()))) return false;
        output = std::move(points);
        return true;
    }
}
#endif
