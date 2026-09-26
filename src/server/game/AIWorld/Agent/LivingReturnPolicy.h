/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AIWORLD_LIVINGRETURNPOLICY_H
#define AIWORLD_LIVINGRETURNPOLICY_H

#include "Action/ActionPosition.h"
#include "Action/ArrivalTolerance.h"
#include "NavigationDiagnostics.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace LivingReturnPolicy
{
    // Stay below the 30-yard path gate. At Elwynn coordinates, a nominal
    // 30-yard float step can round outward and fail that gate forever.
    constexpr float MaxStepLength = 28.0f;
    constexpr std::size_t MaxTrailPoints = 64;

    enum Rejection : std::size_t { Invalid, Height, Zone, Los, Path, Bounds, Danger, RejectionCount };
    struct Diagnostics
    {
        uint32 Candidates = 0;
        std::array<uint32, RejectionCount> Rejected{};
        uint32 PathType = 0;
        float RequestedZ = 0.0f;
        std::optional<float> ResolvedZ;
        NavigationDiagnostics Navigation;
        std::optional<ActionPosition> QueryDestination;
    };

    inline bool Finite(ActionPosition const& point)
    {
        return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
    }

    inline float Distance(ActionPosition const& a, ActionPosition const& b)
    {
        return std::hypot(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    }

    // Check the engine's resolved endpoint, not the proposed height. A trail
    // point on another floor can resolve right back onto the actor itself.
    inline bool UsefulStep(ActionPosition const& from, ActionPosition const& resolved)
    {
        return Finite(from) && Finite(resolved) && from.MapId == resolved.MapId && Distance(from, resolved) > 1.0f;
    }

    // Fixed per episode, with room to walk around a wall. The independent
    // Elwynn and collision checks still apply to every executed segment.
    inline float HomeLimit(float initialDistance)
    { return std::isfinite(initialDistance) ? std::max(96.0f, initialDistance + 64.0f) : 0.0f; }

    inline std::vector<ActionPosition> Corridor(std::vector<ActionPosition> const& path)
    {
        std::vector<ActionPosition> result;
        if (path.size() < 2 || !Finite(path.front())) return result;
        float total = 0;
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            auto const& a = path[i-1]; auto const& b = path[i];
            if (!Finite(b) || a.MapId != b.MapId) return {};
            float length = Distance(a, b);
            total += length;
            if (!std::isfinite(total) || total > 480.0f) return {};
            if (length <= 0.01f) continue;
            unsigned steps = unsigned(std::ceil(length / 14.0f));
            for (unsigned n = 1; n <= steps; ++n)
            {
                float t = float(n) / steps;
                result.push_back({a.MapId, a.X+(b.X-a.X)*t, a.Y+(b.Y-a.Y)*t, a.Z+(b.Z-a.Z)*t});
                if (result.size() > 128) return {};
            }
        }
        return result;
    }

    // A short connector to an existing ground polygon is permitted only over
    // continuously supported, visible terrain. No projection across a cliff,
    // to another floor, or through a wall. Callbacks use the live map geometry.
    template<class HeightAt, class ClearSegment>
    std::vector<ActionPosition> SurfaceConnector(ActionPosition const& from, ActionPosition const& to,
        HeightAt&& heightAt, ClearSegment&& clearSegment, char const** failure = nullptr)
    {
        auto reject = [&](char const* reason) -> std::vector<ActionPosition>
        { if (failure) *failure = reason; return {}; };
        if (failure) *failure = "NONE";
        if (!UsefulStep(from, to)) return reject("CONNECTOR_ZERO_STEP");
        float length = std::hypot(to.X - from.X, to.Y - from.Y);
        if (length < 0.5f || length > 6.0f || std::abs(to.Z - from.Z) > 3.0f) return reject("CONNECTOR_RANGE");
        auto startHeight = heightAt(from);
        if (!startHeight || !std::isfinite(*startHeight) || std::abs(*startHeight - from.Z) > 1.0f) return reject("CONNECTOR_START_HEIGHT");
        std::vector<ActionPosition> result{from};
        unsigned steps = unsigned(std::ceil(length / 0.5f));
        float previousHeight = *startHeight;
        for (unsigned i = 1; i <= steps; ++i)
        {
            float t = float(i) / float(steps);
            ActionPosition point{from.MapId, from.X + (to.X - from.X) * t,
                from.Y + (to.Y - from.Y) * t, from.Z + (to.Z - from.Z) * t};
            auto height = heightAt(point);
            if (!height || !std::isfinite(*height) || std::abs(*height - point.Z) > 1.0f) return reject("CONNECTOR_SURFACE_HEIGHT");
            if (std::abs(*height - previousHeight) > 0.75f) return reject("CONNECTOR_CLIFF");
            point.Z = *height;
            if (!clearSegment(result.back(), point)) return reject("CONNECTOR_OBSTACLE");
            result.push_back(point);
            previousHeight = *height;
        }
        return result;
    }

    template<class InWater, class ClearSegment>
    std::vector<ActionPosition> WaterConnector(ActionPosition const& from, ActionPosition const& to,
        bool canEnterWater, InWater&& inWater, ClearSegment&& clearSegment)
    {
        if (!canEnterWater || !UsefulStep(from, to) || Distance(from, to) > 30.0f) return {};
        unsigned steps = unsigned(std::ceil(Distance(from, to) / 0.5f));
        std::vector<ActionPosition> result;
        ActionPosition previous = from;
        for (unsigned i = 0; i <= steps; ++i)
        {
            float t = float(i) / float(steps);
            ActionPosition point{from.MapId, from.X + (to.X - from.X) * t,
                from.Y + (to.Y - from.Y) * t, from.Z + (to.Z - from.Z) * t};
            if (!inWater(point) || !clearSegment(previous, point)) return {};
            result.push_back(point);
            previous = point;
        }
        return result;
    }

    struct RouteMemory
    {
        bool FollowingTrail = false;
        std::optional<ActionPosition> TrailTarget;
        ActionPosition TrailDestination;
        std::vector<ActionPosition> Visited;
        struct Backtrack { ActionPosition From, To; };
        std::vector<Backtrack> Backtracks;
        std::optional<Backtrack> PendingBacktrack;
        uint64 NextCareAtMs = 0;
        std::vector<Backtrack> FailedEdges;
        std::vector<ActionPosition> Planned;

        bool Failed(ActionPosition const& from, ActionPosition const& to) const
        {
            return std::any_of(FailedEdges.begin(), FailedEdges.end(), [&](Backtrack const& edge)
            { return edge.From.MapId == from.MapId && Distance(edge.From, from) <= 2.0f && Distance(edge.To, to) <= 1.0f; });
        }
        void Reject(ActionPosition const& from, ActionPosition const& to)
        {
            if (!Finite(from) || !Finite(to) || Failed(from, to)) return;
            if (FailedEdges.size() == MaxTrailPoints) FailedEdges.erase(FailedEdges.begin());
            FailedEdges.push_back({from, to});
            Planned.clear();
        }
        void Advance(ActionPosition const& here)
        {
            while (!Planned.empty() && Planned.front().MapId == here.MapId &&
                Distance(Planned.front(), here) <= ArrivalToleranceYards)
                Planned.erase(Planned.begin());
        }

        bool CanBacktrack(ActionPosition const& from, ActionPosition const& to) const
        {
            // Only a fallback after all new routes fail. Each directed edge
            // once, at most 16 in a return episode; movement does not reset it.
            return UsefulStep(from, to) && Backtracks.size() < 16 &&
                std::none_of(Backtracks.begin(), Backtracks.end(), [&](Backtrack const& edge)
                { return edge.From.MapId == from.MapId && Distance(edge.From, from) <= 2.0f &&
                    Distance(edge.To, to) <= 2.0f; });
        }

        void CommitBacktrack()
        {
            if (PendingBacktrack && CanBacktrack(PendingBacktrack->From, PendingBacktrack->To))
                Backtracks.push_back(*PendingBacktrack);
            PendingBacktrack.reset();
        }

        void Remember(ActionPosition const& here)
        {
            if (!Finite(here)) return;
            if (!Visited.empty() && Visited.back().MapId != here.MapId) Visited.clear();
            if (!Visited.empty() && Distance(Visited.back(), here) <= 1.0f) return;
            if (Visited.size() == MaxTrailPoints) Visited.erase(Visited.begin());
            Visited.push_back(here);
        }

        bool Revisited(ActionPosition const& resolved) const
        {
            return std::any_of(Visited.begin(), Visited.end(), [&](ActionPosition const& point)
            { return point.MapId == resolved.MapId && Distance(point, resolved) <= 1.0f; });
        }

        // Call only after real displacement. Consume the original breadcrumb
        // using arrival at its resolved destination, even if its Z changed.
        void ArriveOnTrail(ActionPosition const& here, std::vector<ActionPosition>& trail)
        {
            if (!TrailTarget || !Finite(here) || here.MapId != TrailDestination.MapId ||
                Distance(here, TrailDestination) > ArrivalToleranceYards) return;
            for (std::size_t i = 0; i < trail.size(); ++i)
                if (trail[i].MapId == TrailTarget->MapId && Distance(trail[i], *TrailTarget) < 0.01f)
                {
                    trail.resize(i);
                    break;
                }
            TrailTarget.reset();
        }

        bool NeedsPause(uint64 nowMs, float hunger, float fatigue) const
        {
            return nowMs >= NextCareAtMs && (hunger >= 0.65f || fatigue >= 0.7f);
        }
    };

    // Actual observed positions only. Revisited points erase loops; recovery
    // consumes the trail instead of recording its own steps back out again.
    inline void ObserveTrail(std::vector<ActionPosition>& trail, ActionPosition const& point, bool returning)
    {
        if (!Finite(point)) return;
        if (!trail.empty() && trail.back().MapId != point.MapId) trail.clear();
        for (std::size_t i = 0; i < trail.size(); ++i)
            if (Distance(trail[i], point) <= 2.0f)
            {
                trail.resize(i + 1);
                return;
            }
        if (returning || (!trail.empty() && Distance(trail.back(), point) < 3.0f)) return;
        if (trail.size() == MaxTrailPoints) trail.erase(trail.begin() + 1);
        trail.push_back(point);
    }

    inline std::vector<ActionPosition> TrailSteps(std::vector<ActionPosition> const& trail, ActionPosition const& from)
    {
        std::vector<ActionPosition> candidates;
        if (!Finite(from)) return candidates;
        for (auto it = trail.rbegin(); it != trail.rend(); ++it)
            if (Finite(*it) && it->MapId == from.MapId && Distance(*it, from) > 2.0f && Distance(*it, from) <= MaxStepLength)
            {
                candidates.push_back(*it);
                if (candidates.size() == 8) break;
            }
        return candidates;
    }

    inline uint64 RetryDelayMs(uint32 failures)
    {
        return std::min<uint64>(60000, 5000ULL << std::min(failures ? failures - 1 : 0, 4u));
    }

    // Distinct rings/directions on successive attempts, including a bounded
    // first step away from home when a wall requires walking around it.
    inline std::vector<ActionPosition> Detours(ActionPosition const& from, ActionPosition const& home, uint32 attempt)
    {
        std::vector<ActionPosition> candidates;
        float distance = std::hypot(home.X - from.X, home.Y - from.Y);
        if (from.MapId != home.MapId || !Finite(from) || !Finite(home) || !std::isfinite(distance) || distance <= 2.0f)
            return candidates;
        float bearing = std::atan2(home.Y - from.Y, home.X - from.X);
        float step = 3.0f + 3.0f * (attempt % 4);
        float rotation = float(attempt % 8) * 0.19634954f;
        for (uint32 i = 0; i < 8; ++i)
        {
            float angle = bearing + rotation + float(i) * 0.78539816f;
            candidates.push_back({ from.MapId, from.X + step * std::cos(angle),
                from.Y + step * std::sin(angle), from.Z });
        }
        return candidates;
    }

    // Follow the route, rather than projecting a straight chord toward home.
    // The engine still validates its floor, full path and zone; shortcuts
    // without navmesh also require direct line of sight.
    inline std::optional<ActionPosition> PathStep(std::vector<ActionPosition> const& path, float budget = MaxStepLength)
    {
        auto finite = [](ActionPosition const& point)
        { return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z); };
        if (path.size() < 2 || !finite(path.front()) || !std::isfinite(budget) || budget <= 0.0f || budget > MaxStepLength)
            return std::nullopt;
        ActionPosition cursor = path.front();
        bool advanced = false;
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            ActionPosition const& next = path[i];
            if (next.MapId != cursor.MapId || !finite(next))
                return std::nullopt;
            float dx = next.X - cursor.X, dy = next.Y - cursor.Y, dz = next.Z - cursor.Z;
            float length = std::hypot(dx, dy, dz);
            if (!std::isfinite(length))
                return std::nullopt;
            if (length > 0.0f)
            {
                if (length >= budget)
                {
                    float part = budget / length;
                    return ActionPosition{ cursor.MapId, cursor.X + dx * part, cursor.Y + dy * part, cursor.Z + dz * part };
                }
                budget -= length;
                advanced = true;
            }
            cursor = next;
        }
        return advanced ? std::optional<ActionPosition>(cursor) : std::nullopt;
    }
}

#endif
