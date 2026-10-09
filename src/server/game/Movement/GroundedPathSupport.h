/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_GROUNDEDPATHSUPPORT_H
#define TRINITY_GROUNDEDPATHSUPPORT_H

#include "MovementPathBounds.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Movement
{
    constexpr float GroundedPathStep = 0.5f;
    constexpr float GroundedPathLengthLimit = 64.0f;
    // Includes the real source. This bounds floor/tile/body callbacks and
    // keeps the executed spline bounded even when the source path has loops.
    constexpr std::size_t GroundedPathSampleLimit = 128;

    // Match MoveSplineInit::Launch for a world-space source without moving
    // or stopping the actor. Map cell crossings can defer its cached XYZ
    // update even after Unit::UpdateSplinePosition has been called.
    template<class Point, class CurrentSplinePosition>
    bool SelectPhysicalSplineSource(Point const& cached, bool transport,
        bool finalized, bool splineOnTransport, CurrentSplinePosition&& current, Point& source)
    {
        if (transport) return false;
        source = !finalized && !splineOnTransport ? current() : cached;
        return std::isfinite(source.x) && std::isfinite(source.y) && std::isfinite(source.z);
    }

    // An actor's swimming capability or one wet endpoint cannot authorize
    // skipping support for a dry segment. Preserve swimming navigation only
    // after the same bounded whole-path sampling proves every point wet.
    template<class Points, class IsWater>
    bool FullySwimmingPath(Points const& points, IsWater&& isWater)
    {
        return points.size() >= 2 && PathWithinBounds(points,[&](float x,float y,float z)
        {
            auto point = points.front(); point.x = x; point.y = y; point.z = z;
            return isWater(point);
        });
    }

    // Use after all navmesh shortening, on dry ground routes only. A complete
    // mesh path and collision-free endpoints do not prove support between
    // its four-yard vertices or at an interpolated shortened endpoint.
    // Resolve each half-yard point from the preceding physical floor, not
    // from a potentially elevated mesh vertex. The published dense controls
    // keep an interrupted linear spline close to that checked floor too.
    // Callbacks return actor-foot heights including any legitimate hover
    // offset; tile checks refer to installed XY tiles, not mesh projection.
    // Raw and output vectors must be distinct; failure always clears output.
    template<class Points, class Point, class HeightAt, class ClearBody, class Contains, class HasTile>
    bool PrepareGroundedPath(Points const& raw, Point const& origin, Points& output,
        HeightAt&& heightAt, ClearBody&& clearBody, Contains&& contains, HasTile&& hasTile,
        char const** failure = nullptr)
    {
        output.clear();
        auto reject = [&](char const* reason)
        { if (failure) *failure = reason; return false; };
        auto finite = [](auto const& p)
        { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
        if (failure) *failure = "NONE";
        if (!finite(origin) || raw.size() < 2 || raw.size() > GroundedPathSampleLimit)
            return reject("GROUND_PATH_LIMIT");
        if (!finite(raw.front()) || raw.front().x != origin.x || raw.front().y != origin.y || raw.front().z != origin.z)
            return reject("GROUND_PATH_SOURCE");

        // Reject excessive work before any map callback. Both the original
        // and the resulting physical path must obey the three-dimensional
        // length limit; budgets never authorize an unpublished prefix.
        std::array<std::size_t, GroundedPathSampleLimit> samples{};
        std::size_t count = 1;
        float rawLength = 0;
        for (std::size_t i = 1; i < raw.size(); ++i)
        {
            auto const& a = raw[i-1]; auto const& b = raw[i];
            if (!finite(b)) return reject("GROUND_PATH_COORDINATES");
            float length = std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);
            rawLength += length;
            if (!std::isfinite(rawLength) || rawLength > GroundedPathLengthLimit)
                return reject("GROUND_PATH_LIMIT");
            float horizontal = std::hypot(b.x-a.x,b.y-a.y);
            // A vertical off-mesh link is not an ordinary walking edge.
            if (horizontal < 0.001f && std::abs(b.z-a.z) > 0.75f)
                return reject("GROUND_PATH_VERTICAL");
            // Repeated mesh vertices have no swept body or new XY floor.
            // Keeping them as samples would reintroduce a zero-length source
            // check with an arbitrary axis; real vertical links were rejected
            // above, and the source/next moving segment retain all their checks.
            samples[i] = horizontal > 0 ? std::size_t(std::ceil(horizontal/GroundedPathStep)) : 0;
            if (samples[i] > GroundedPathSampleLimit-count) return reject("GROUND_PATH_LIMIT");
            count += samples[i];
        }
        if (count < 2) return reject("GROUND_PATH_LIMIT");

        if (!hasTile(origin) || !contains(origin)) return reject("GROUND_PATH_BOUNDS");
        auto support = heightAt(origin);
        if (!support || !std::isfinite(*support) || std::abs(*support-origin.z) > 1.0f)
            return reject("GROUND_PATH_START_HEIGHT");
        // The first sampled segment checks the body's source and destination
        // in its actual travel direction, including both vertical clearances.
        // A separate zero-length sweep has no direction: BodyClear chooses an
        // arbitrary lateral axis that can cut into a supported uphill floor
        // even when the incoming and outgoing sweeps are both unobstructed.
        Points grounded;
        grounded.reserve(count);
        grounded.push_back(origin); // no source projection or relocation
        float previousHeight = *support, length = 0;
        for (std::size_t i = 1; i < raw.size(); ++i)
        {
            auto const& a = raw[i-1]; auto const& b = raw[i];
            for (std::size_t sample = 1; sample <= samples[i]; ++sample)
            {
                float t = float(sample)/float(samples[i]);
                auto point = b;
                point.x = a.x+(b.x-a.x)*t;
                point.y = a.y+(b.y-a.y)*t;
                point.z = previousHeight;
                if (!hasTile(point)) return reject("GROUND_PATH_MISSING_TILE");
                auto height = heightAt(point);
                if (!height || !std::isfinite(*height)) return reject("GROUND_PATH_HEIGHT");
                if (std::abs(*height-previousHeight) > 0.75f) return reject("GROUND_PATH_CLIFF");
                point.z = *height;
                if (!contains(point)) return reject("GROUND_PATH_BOUNDS");
                auto const& previous = grounded.back();
                if (!clearBody(previous,point)) return reject("GROUND_PATH_OBSTACLE");
                length += std::hypot(point.x-previous.x,point.y-previous.y,point.z-previous.z);
                if (!std::isfinite(length) || length > GroundedPathLengthLimit)
                    return reject("GROUND_PATH_LIMIT");
                grounded.push_back(point); previousHeight = *height;
            }
        }
        output = std::move(grounded);
        return true;
    }
}
#endif
