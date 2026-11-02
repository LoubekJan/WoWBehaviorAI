/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGFINEROUTE_H
#define AIWORLD_LIVINGFINEROUTE_H
#include "LivingSurfaceCorridor.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace LivingFineRoute
{
    constexpr std::size_t MaxControls = 17, MaxDensePoints = 64;
    // Leave room for real supported-floor curvature between graph controls.
    // The freshly grounded spline still has the independent strict6yd cap.
    constexpr float SelectionLength = 5.0f, MaxLength = 6.0f, SourceTolerance = 0.05f;
    inline float Distance(ActionPosition const& a, ActionPosition const& b)
    { return std::hypot(a.X-b.X,a.Y-b.Y,a.Z-b.Z); }
    inline bool Same(ActionPosition const& a, ActionPosition const& b)
    { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; }

    // Select a contiguous prefix. Short turns survive even when no individual
    // edge satisfies the generic >1yd request gate. No skipped or new corner.
    inline std::vector<ActionPosition> Batch(ActionPosition const& from, std::vector<ActionPosition> const& planned)
    {
        std::vector<ActionPosition> batch{from};
        float length = 0;
        std::size_t useful = 0;
        if (!LivingSurfaceCorridor::Finite(from)) return {};
        for (std::size_t n = 0; n < planned.size(); ++n)
        {
            auto const& point = planned[n];
            if (!LivingSurfaceCorridor::Finite(point) || point.MapId != from.MapId) return {};
            float segment = Distance(batch.back(),point);
            if (segment <= 0.001f) continue;
            if (batch.size() == MaxControls || length+segment > SelectionLength) break;
            batch.push_back(point); length += segment;
            // Do not end a batch one short turn before the final goal. Such
            // an unstarted sub-yard tail still cannot pass UsefulStep; leave
            // an earlier corner so the next complete batch has net progress.
            if (Distance(from,point) > 1.0f &&
                (n+1 == planned.size() || Distance(point,planned.back()) > 1.0f)) useful = batch.size();
        }
        if (!useful) return {};
        batch.resize(useful);
        return batch;
    }

    inline bool Bounded(std::vector<ActionPosition> const& controls,std::size_t controlLimit = MaxControls)
    {
        if ((controlLimit != MaxControls && controlLimit != MaxDensePoints) || controls.size() < 2 || controls.size() > controlLimit ||
            !LivingSurfaceCorridor::Finite(controls.front()) || Distance(controls.front(),controls.back()) <= 1.0f) return false;
        float length = 0;
        for (std::size_t i = 1; i < controls.size(); ++i)
        {
            auto const& point = controls[i];
            if (!LivingSurfaceCorridor::Finite(point) || point.MapId != controls.front().MapId) return false;
            float segment = Distance(controls[i-1],point);
            if (segment <= 0.001f || (length += segment) > MaxLength) return false;
        }
        return true;
    }

    inline bool RetainsCorners(std::vector<ActionPosition> const& original,std::vector<ActionPosition> const& dense)
    {
        if (!Bounded(original) || !Bounded(dense,MaxDensePoints) ||
            original.front().MapId != dense.front().MapId || Distance(original.front(),dense.front()) > SourceTolerance) return false;
        std::size_t next = 1;
        for (std::size_t i = 1; i < original.size(); ++i)
        {
            while (next < dense.size() && !(dense[next].X == original[i].X && dense[next].Y == original[i].Y &&
                std::abs(dense[next].Z-original[i].Z) <= 1.0f)) ++next;
            if (next == dense.size()) return false;
            ++next;
        }
        return next == dense.size();
    }

    // A speed change may start inside a proved segment. Resume at the first
    // matching segment, retaining every later bend. Off-route displacement
    // invalidates the proof instead of manufacturing a connector/chord.
    inline std::vector<ActionPosition> Remaining(ActionPosition const& from, std::vector<ActionPosition> const& controls,
        std::size_t controlLimit = MaxControls)
    {
        if (!Bounded(controls,controlLimit) || !LivingSurfaceCorridor::Finite(from) || from.MapId != controls.front().MapId) return {};
        for (std::size_t i = 1; i < controls.size(); ++i)
        {
            auto const& a = controls[i-1]; auto const& b = controls[i];
            float dx = b.X-a.X, dy = b.Y-a.Y, dz = b.Z-a.Z;
            float square = dx*dx+dy*dy+dz*dz;
            float t = std::clamp(((from.X-a.X)*dx+(from.Y-a.Y)*dy+(from.Z-a.Z)*dz)/square,0.0f,1.0f);
            ActionPosition projection{from.MapId,a.X+t*dx,a.Y+t*dy,a.Z+t*dz};
            if (Distance(from,projection) > SourceTolerance) continue;
            std::vector<ActionPosition> result{from};
            for (std::size_t n = i; n < controls.size(); ++n)
                if (Distance(result.back(),controls[n]) > 0.001f) result.push_back(controls[n]);
            return result;
        }
        return {};
    }

    // Every original segment is proved again, including floor samples at most
    // half a yard apart. The output contains all original XY corners and the
    // freshly supported height; it never substitutes an endpoint chord.
    template<class HeightAt, class ClearSegment, class Contains, class HasTile>
    std::vector<ActionPosition> Revalidate(ActionPosition const& from, std::vector<ActionPosition> const& controls,
        HeightAt&& heightAt, ClearSegment&& clearSegment, Contains&& contains, HasTile&& hasTile,
        char const** failure = nullptr,std::size_t controlLimit = MaxControls)
    {
        auto reject = [&](char const* reason) -> std::vector<ActionPosition>
        { if (failure) *failure = reason; return {}; };
        auto remaining = Remaining(from,controls,controlLimit);
        if (remaining.size() < 2) return reject("FINE_ROUTE_SOURCE");
        std::vector<ActionPosition> result{from};
        float executedLength = 0;
        for (std::size_t i = 1; i < remaining.size(); ++i)
        {
            LivingSurfaceCorridor::Search segment;
            auto state = LivingSurfaceCorridor::Advance(segment,result.back(),remaining[i],heightAt,
                clearSegment,contains,12,hasTile);
            if (state != LivingSurfaceCorridor::Status::Complete) return reject(segment.Failure);
            if (result.size()+segment.Route.size()-1 > MaxDensePoints) return reject("FINE_ROUTE_POINTS");
            for (std::size_t n = 1; n < segment.Route.size(); ++n)
                if ((executedLength += Distance(segment.Route[n-1],segment.Route[n])) > MaxLength)
                    return reject("FINE_ROUTE_RANGE");
            result.insert(result.end(),segment.Route.begin()+1,segment.Route.end());
        }
        if (failure) *failure = "NONE";
        return result;
    }
}
#endif
