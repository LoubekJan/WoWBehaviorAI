/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGSURFACECORRIDOR_H
#define AIWORLD_LIVINGSURFACECORRIDOR_H
#include "Action/ActionPosition.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace LivingSurfaceCorridor
{
    struct InstalledTiles
    { bool operator()(ActionPosition const&) const { return true; } };
    enum class Status { Pending, Complete, Rejected };
    struct Search
    {
        Status State = Status::Pending;
        char const* Failure = "NOT_CHECKED";
        ActionPosition From, Target;
        std::vector<ActionPosition> Route;
        unsigned NextSample = 1, Samples = 0;
        float PreviousHeight = 0, Length = 0;
        bool Started = false;
    };
    inline bool Finite(ActionPosition const& p)
    { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }

    // A bounded approximation of the actor's body, not just its centre ray.
    // Three lateral tracks and three heights also catch narrow side walls and
    // beams between foot and head. Vertical checks cover endpoint clearance.
    template<class ClearRay>
    bool BodyClear(ActionPosition const& a, ActionPosition const& b, float radius, float height, ClearRay&& clearRay)
    {
        if (!Finite(a) || !Finite(b) || !std::isfinite(radius) || !std::isfinite(height) ||
            radius < 0 || height < 0) return false;
        height = std::max(0.5f, height);
        float length = std::hypot(b.X-a.X, b.Y-a.Y);
        float sideX = length > 0.001f ? -(b.Y-a.Y)/length : 1.0f;
        float sideY = length > 0.001f ? (b.X-a.X)/length : 0.0f;
        for (float offset : {-radius, 0.0f, radius})
        {
            ActionPosition left = a, right = b;
            left.X += sideX*offset; left.Y += sideY*offset;
            right.X += sideX*offset; right.Y += sideY*offset;
            for (float lift : {0.3f, std::max(0.3f, height*0.5f), height})
            {
                auto from = left, to = right;
                from.Z += lift; to.Z += lift;
                if (!Finite(from) || !Finite(to) || !clearRay(from, to)) return false;
            }
            for (auto const& point : {left, right})
            {
                auto foot = point, head = point;
                foot.Z += 0.3f; head.Z += height;
                if (!Finite(foot) || !Finite(head) || !clearRay(foot, head)) return false;
            }
        }
        return true;
    }

    // A terrain fallback is a complete planned route, never permission to
    // launch an incomplete navmesh path. Each half-yard segment is checked
    // against the real floor, collisions and the caller's unchanged bounds.
    template<class HeightAt, class ClearSegment, class Contains, class HasTile = InstalledTiles>
    Status Advance(Search& search, ActionPosition const& from, ActionPosition const& target,
        HeightAt&& heightAt, ClearSegment&& clearSegment, Contains&& contains, unsigned sampleBudget = 8,
        HasTile&& hasTile = {})
    {
        auto reject = [&](char const* reason)
        { search.Failure = reason; search.State = Status::Rejected; search.Route.clear(); return search.State; };
        auto same = [](ActionPosition const& a, ActionPosition const& b)
        { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; };
        if (search.Started && (!same(search.From, from) || !same(search.Target, target))) return reject("SURFACE_CONTEXT");
        if (search.State != Status::Pending) return search.State;
        if (!search.Started)
        {
            search.Started = true; search.From = from; search.Target = target;
            if (!Finite(from) || !Finite(target) || from.MapId != target.MapId ||
                !contains(from) || !contains(target)) return reject("SURFACE_BOUNDS");
            if (!hasTile(from) || !hasTile(target)) return reject("SURFACE_MISSING_TILE");
            float distance = std::hypot(target.X-from.X, target.Y-from.Y);
            if (distance < 0.01f || distance > 480.0f) return reject("SURFACE_RANGE");
            auto height = heightAt(from);
            if (!height || !std::isfinite(*height) || std::abs(*height-from.Z) > 1.0f)
                return reject("SURFACE_START_HEIGHT");
            search.PreviousHeight = *height;
            search.Samples = unsigned(std::ceil(distance / 0.5f));
            search.Route.push_back(from);
            search.Failure = "NONE";
        }
        unsigned until = std::min(search.Samples + 1, search.NextSample + sampleBudget);
        for (; search.NextSample < until; ++search.NextSample)
        {
            float t = float(search.NextSample) / search.Samples;
            ActionPosition point{from.MapId, from.X+(target.X-from.X)*t,
                from.Y+(target.Y-from.Y)*t, search.PreviousHeight};
            if (!hasTile(point)) return reject("SURFACE_MISSING_TILE");
            auto height = heightAt(point);
            if (!height || !std::isfinite(*height)) return reject("SURFACE_HEIGHT");
            if (std::abs(*height-search.PreviousHeight) > 0.75f) return reject("SURFACE_CLIFF");
            point.Z = *height;
            if (!contains(point)) return reject("SURFACE_BOUNDS");
            auto const& previous = search.Route.back();
            if (!clearSegment(previous, point)) return reject("SURFACE_OBSTACLE");
            search.Length += std::hypot(point.X-previous.X, point.Y-previous.Y, point.Z-previous.Z);
            if (search.Length > 480.0f) return reject("SURFACE_RANGE");
            search.Route.push_back(point);
            search.PreviousHeight = *height;
        }
        if (search.NextSample <= search.Samples) return search.State;
        if (std::abs(search.Route.back().Z-target.Z) > 1.0f) return reject("SURFACE_END_HEIGHT");
        search.State = Status::Complete;
        return search.State;
    }

    // Group only already-validated samples into short execution legs. Every
    // launched leg is sampled again from the live position by the provider.
    inline std::vector<ActionPosition> Legs(std::vector<ActionPosition> const& route)
    {
        std::vector<ActionPosition> result;
        if (route.size() < 2) return result;
        float length = 0;
        for (std::size_t i = 1; i < route.size(); ++i)
        {
            auto const& previous = route[i-1]; auto const& point = route[i];
            float segment = std::hypot(point.X-previous.X, point.Y-previous.Y, point.Z-previous.Z);
            // Bound the complete three-dimensional execution leg. Height
            // changes follow the checked slope, not a projection jump.
            if (length > 0 && length+segment > 5.5f)
            { result.push_back(previous); length = 0; }
            length += segment;
            if (length >= 5.0f || i+1 == route.size())
            { result.push_back(point); length = 0; }
        }
        return result;
    }
}
#endif
