/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#ifndef TRINITY_MOVEMENTPATHBOUNDS_H
#define TRINITY_MOVEMENTPATHBOUNDS_H

#include <cmath>
#include <cstddef>

namespace Movement
{
    // Checking only navmesh corners misses a segment cutting across a zone
    // boundary. Bound both sampling distance and work before launching it.
    template<class Path, class Contains>
    bool PathWithinBounds(Path const& path, Contains&& contains)
    {
        if (path.empty())
            return false;
        auto const& first = path.front();
        if (!std::isfinite(first.x) || !std::isfinite(first.y) || !std::isfinite(first.z) ||
            !contains(first.x, first.y, first.z))
            return false;
        std::size_t remaining = 512;
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            auto const& a = path[i - 1];
            auto const& b = path[i];
            float length = std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
            if (!std::isfinite(length) || length > float(remaining))
                return false;
            std::size_t steps = length > 0.0f ? std::size_t(std::ceil(length)) : 1;
            if (steps > remaining)
                return false;
            remaining -= steps;
            for (std::size_t step = 1; step <= steps; ++step)
            {
                float t = float(step) / float(steps);
                if (!contains(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t))
                    return false;
            }
        }
        return true;
    }
}
#endif
