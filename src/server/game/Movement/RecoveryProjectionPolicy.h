/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_RECOVERYPROJECTIONPOLICY_H
#define TRINITY_RECOVERYPROJECTIONPOLICY_H
#include <cstddef>
#include <cmath>
#include <optional>

namespace RecoveryProjectionPolicy
{
    struct Point { float X, Y, Z; };
    struct Candidate { Point Polygon, Ground; };

    // Navmesh vertices include voxel/agent clearance offsets. They identify
    // a polygon, not the feet of an executable waypoint. Resolve the physical
    // surface using the actor's floor seed, then apply the connector's real
    // six-yard/three-yard limits. A different floor cannot be inferred merely
    // from the presence of a polygon.
    template<class HeightAt>
    std::optional<Point> Resolve(Point const& from, Point const& polygon, HeightAt&& heightAt,
        char const** failure = nullptr)
    {
        auto reject = [&](char const* reason) -> std::optional<Point>
        { if (failure) *failure = reason; return std::nullopt; };
        if (!std::isfinite(from.X) || !std::isfinite(from.Y) || !std::isfinite(from.Z) ||
            !std::isfinite(polygon.X) || !std::isfinite(polygon.Y) || !std::isfinite(polygon.Z))
            return reject("INVALID_PROJECTION");
        if (std::hypot(polygon.X-from.X, polygon.Y-from.Y) > 6.0f) return reject("PROJECTION_RANGE");
        auto height = heightAt(Point{polygon.X, polygon.Y, from.Z});
        if (!height || !std::isfinite(*height) || std::abs(*height-polygon.Z) > 3.0f)
            return reject("PROJECTION_GROUND_HEIGHT");
        if (std::abs(*height-from.Z) > 3.0f) return reject("PROJECTION_HEIGHT");
        if (failure) *failure = "NONE";
        return Point{polygon.X, polygon.Y, *height};
    }

    template<class HeightAt, class HasTile>
    std::optional<Candidate> Nearest(Point const& from, Point const& center, Point const* polygons,
        std::size_t count, HeightAt&& heightAt, HasTile&& hasTile, char const** failure = nullptr)
    {
        std::optional<Candidate> result;
        float best = 0;
        char const* reason = "NO_GROUND_POLYGON";
        for (std::size_t i = 0; i < count; ++i)
        {
            auto ground = Resolve(from, polygons[i], heightAt, &reason);
            if (!ground) continue;
            if (!hasTile(*ground)) { reason = "MISSING_PROJECTION_TILE"; continue; }
            float distance = std::hypot(ground->X-center.X, ground->Y-center.Y, ground->Z-from.Z);
            if (result && distance >= best) continue;
            best = distance;
            result = Candidate{polygons[i], *ground};
        }
        if (failure) *failure = result ? "NONE" : reason;
        return result;
    }
}
#endif
