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
#include "Agent/LivingSurfaceCorridor.h"
#include "Agent/LivingSurfaceDetour.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace LivingReturnPolicy
{
    // Stay below the 30-yard path gate. At Elwynn coordinates, a nominal
    // 30-yard float step can round outward and fail that gate forever.
    constexpr float MaxStepLength = 28.0f;
    constexpr std::size_t MaxTrailPoints = 64;

    // Stay strictly inside the existing arrival radius, rather than requiring
    // the exact spawn point to lie on a reachable navigation polygon.
    inline std::vector<ActionPosition> HomeTargets(ActionPosition const& home, float arrivalRadius)
    {
        if (!std::isfinite(arrivalRadius) || arrivalRadius < 0) return {};
        std::vector<ActionPosition> targets{home};
        float ring = std::max(0.0f, arrivalRadius - 1.0f);
        if (ring < 1) return targets;
        for (unsigned i = 0; i < 8; ++i)
        {
            float angle = float(i) * 0.785398163f;
            targets.push_back({home.MapId, home.X + ring * std::cos(angle), home.Y + ring * std::sin(angle), home.Z});
        }
        return targets;
    }

    enum Rejection : std::size_t { Invalid, Height, Zone, Los, Path, Bounds, Danger, RejectionCount };
    enum class HomePathFailure : std::size_t { Ground, NoPath, Endpoint, Bounds, Danger, Corridor, Count };
    struct HomePathReport
    {
        bool SurfaceCorridor = false;
        std::string SurfaceFailure = "NOT_CHECKED";
        std::string DetourFailure = "NOT_CHECKED";
        uint32 DetourNodes = 0, DetourEdges = 0;
        std::array<uint32, std::size_t(HomePathFailure::Count)> Rejected{};
        uint32 PathType = 0;
        std::string Failure = "NOT_CHECKED";
        std::size_t Stage = 0;

        void Reject(HomePathFailure stage, uint32 pathType)
        {
            std::size_t index = std::size_t(stage);
            ++Rejected[index];
            // Keep the attempt which got furthest, not the final ring point
            // whose height may fail before a navmesh query even takes place.
            if (index < Stage) return;
            Stage = index;
            PathType = pathType;
            char const* names[] = {"HOME_GROUND_HEIGHT", "NO_COMPLETE_PATH", "ENDPOINT_MISMATCH",
                "PATH_BOUNDS", "DANGER_BLOCKED", "CORRIDOR_LIMIT"};
            Failure = names[index];
        }
    };
    struct Diagnostics
    {
        bool Deferred = false;
        uint32 Candidates = 0;
        std::array<uint32, RejectionCount> Rejected{};
        uint32 PathType = 0;
        HomePathReport HomePath, ContinuationPath;
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

    inline bool SamePosition(ActionPosition const& a, ActionPosition const& b)
    { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; }

    // A suspended search resumes at the first untested home candidate. Its
    // result is valid only for this exact geometric request; execution still
    // validates from the actor's live position.
    struct HomeCorridorSearch
    {
        HomePathReport Report;
        std::size_t NextTarget = 0;
        bool Done = false, HasContext = false, ArrivalChecked = false;
        std::size_t NextSurfaceTarget = 0;
        std::array<bool, 9> SurfaceEligible{};
        std::array<std::optional<ActionPosition>, 9> GroundTargets{};
        LivingSurfaceCorridor::Search Surface;
        LivingSurfaceDetour::Search Detour;
        bool AllowDetour = false;
        ActionPosition From, Home;
        std::optional<ActionPosition> Danger;
        float ArrivalRadius = 0, Limit = 0, Clearance = 0;

        bool Matches(ActionPosition const& from, ActionPosition const& home, float radius, float limit,
            ActionPosition const* danger, float clearance, bool allowDetour = false) const
        {
            return HasContext && SamePosition(From, from) && SamePosition(Home, home) &&
                ArrivalRadius == radius && Limit == limit && Clearance == clearance && AllowDetour == allowDetour &&
                Danger.has_value() == bool(danger) && (!danger || SamePosition(*Danger, *danger));
        }
        void Begin(ActionPosition const& from, ActionPosition const& home, float radius, float limit,
            ActionPosition const* danger, float clearance, bool allowDetour = false)
        {
            *this = {};
            HasContext = true; From = from; Home = home;
            ArrivalRadius = radius; Limit = limit; Clearance = clearance;
            AllowDetour = allowDetour;
            if (danger) Danger = *danger;
            Report.Failure = "INVALID_REQUEST";
        }
    };

    struct RejoinSearch
    {
        ActionPosition From;
        std::vector<ActionPosition> Candidates;
        NavigationDiagnostics Navigation;
        std::size_t NextProbe = 0;
        bool Done = false, HasContext = false;
    };

    // A ring target carries home's Z only as a seed. If it is on a slope,
    // follow supported terrain from home rather than rejecting the entire
    // accumulated elevation change. This resolves a destination, not a route:
    // the caller must still obtain a complete navmesh path to it.
    template<class HeightAt, class ClearSegment>
    std::optional<ActionPosition> GroundHomeTarget(ActionPosition const& home, ActionPosition target,
        HeightAt&& heightAt, ClearSegment&& clearSegment)
    {
        if (!Finite(home) || !Finite(target) || home.MapId != target.MapId) return std::nullopt;
        float length = std::hypot(target.X-home.X, target.Y-home.Y);
        if (length > 32.0f) return std::nullopt;
        target.Z = home.Z;
        auto height = heightAt(target);
        if (height && std::isfinite(*height) && std::abs(*height-home.Z) <= 3.0f)
        { target.Z = *height; return target; }
        auto homeHeight = heightAt(home);
        if (!homeHeight || !std::isfinite(*homeHeight) || std::abs(*homeHeight-home.Z) > 3.0f || length < 0.01f)
            return std::nullopt;
        ActionPosition previous = home;
        previous.Z = *homeHeight;
        unsigned steps = unsigned(std::ceil(length / 0.5f));
        for (unsigned i = 1; i <= steps; ++i)
        {
            float t = float(i) / float(steps);
            ActionPosition point{home.MapId, home.X+(target.X-home.X)*t, home.Y+(target.Y-home.Y)*t, previous.Z};
            height = heightAt(point);
            if (!height || !std::isfinite(*height) || std::abs(*height-previous.Z) > 0.75f) return std::nullopt;
            point.Z = *height;
            if (!clearSegment(previous, point)) return std::nullopt;
            previous = point;
        }
        return previous;
    }

    inline bool HomeEndpointMatches(ActionPosition const& home, float radius, ActionPosition const& endpoint,
        std::optional<ActionPosition> const& supported)
    {
        return Finite(home) && Finite(endpoint) && home.MapId == endpoint.MapId &&
            std::isfinite(radius) && radius >= 0 && std::hypot(endpoint.X-home.X, endpoint.Y-home.Y) <= radius &&
            supported && Finite(*supported) && supported->MapId == endpoint.MapId &&
            Distance(endpoint, *supported) <= 1.0f;
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
        HeightAt&& heightAt, ClearSegment&& clearSegment, char const** failure = nullptr,
        std::optional<ActionPosition> startSupport = std::nullopt, NavigationDiagnostics* diagnostics = nullptr)
    {
        ActionPosition tested = from;
        std::optional<float> testedGround, previousGround;
        if (diagnostics)
        {
            diagnostics->ConnectorSamples = 0;
            diagnostics->RejectedX.reset(); diagnostics->RejectedY.reset(); diagnostics->RejectedZ.reset();
            diagnostics->RejectedGroundZ.reset(); diagnostics->PreviousGroundZ.reset();
        }
        auto reject = [&](char const* reason) -> std::vector<ActionPosition>
        {
            if (failure) *failure = reason;
            if (diagnostics)
            {
                diagnostics->RejectedX = tested.X; diagnostics->RejectedY = tested.Y; diagnostics->RejectedZ = tested.Z;
                diagnostics->RejectedGroundZ = testedGround; diagnostics->PreviousGroundZ = previousGround;
            }
            return {};
        };
        if (failure) *failure = "NONE";
        if (!UsefulStep(from, to)) return reject("CONNECTOR_ZERO_STEP");
        float length = std::hypot(to.X - from.X, to.Y - from.Y);
        if (length < 0.5f || length > 6.0f || std::abs(to.Z - from.Z) > 3.0f) return reject("CONNECTOR_RANGE");
        auto startHeight = heightAt(from);
        testedGround = startHeight;
        if (!startHeight || !std::isfinite(*startHeight)) return reject("CONNECTOR_START_HEIGHT");
        ActionPosition grounded = from;
        grounded.Z = *startHeight;
        std::vector<ActionPosition> result{from};
        if (std::abs(*startHeight - from.Z) > 1.0f)
        {
            // The caller supplies a ground-normalized nearby polygon. It may
            // be beside an off-mesh actor. The sampled connector below must
            // continuously reach that terrain; proximity in XY alone is not
            // proof of a floor, and raw navmesh Z is not engine ground Z.
            if (!startSupport || !Finite(*startSupport) || startSupport->MapId != from.MapId ||
                std::abs(*startHeight-from.Z) > 3.0f ||
                std::hypot(startSupport->X-from.X, startSupport->Y-from.Y) > 6.0f)
                return reject("CONNECTOR_START_HEIGHT");
            auto supportHeight = heightAt(*startSupport);
            if (!supportHeight || !std::isfinite(*supportHeight) || std::abs(*supportHeight-startSupport->Z) > 1.0f)
                return reject("CONNECTOR_SUPPORT_HEIGHT");
            if (!clearSegment(from, grounded)) return reject("CONNECTOR_START_OBSTACLE");
            result.push_back(grounded);
        }
        unsigned steps = unsigned(std::ceil(length / 0.5f));
        float previousHeight = *startHeight;
        for (unsigned i = 1; i <= steps; ++i)
        {
            float t = float(i) / float(steps);
            ActionPosition point{from.MapId, from.X + (to.X - from.X) * t,
                from.Y + (to.Y - from.Y) * t, previousHeight};
            // Follow the actual surface, not the chord between endpoints.
            // A continuous hollow or crest is not a missing floor.
            auto height = heightAt(point);
            tested = point; testedGround = height; previousGround = previousHeight;
            if (diagnostics) ++diagnostics->ConnectorSamples;
            if (!height || !std::isfinite(*height)) return reject("CONNECTOR_SURFACE_HEIGHT");
            if (std::abs(*height - previousHeight) > 0.75f) return reject("CONNECTOR_CLIFF");
            point.Z = *height;
            if (!clearSegment(result.back(), point)) return reject("CONNECTOR_OBSTACLE");
            result.push_back(point);
            previousHeight = *height;
        }
        if (std::abs(previousHeight - to.Z) > 1.0f) return reject("CONNECTOR_END_HEIGHT");
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
        std::vector<Backtrack> Rejoins;
        std::vector<ActionPosition> Planned;
        bool SurfaceCorridor = false;

        // Shared by deterministic recovery and model candidates. A verified
        // corridor may retrace a visited place. A surface rejoin is a bounded
        // one-shot repair, independent of the speculative detour budget.
        bool Allows(ActionPosition const& from, ActionPosition const& to, bool corridor, bool rejoin) const
        {
            if (!UsefulStep(from, to) || Failed(from, to)) return false;
            if (rejoin)
                return Rejoins.size() < 8 && std::none_of(Rejoins.begin(), Rejoins.end(), [&](Backtrack const& edge)
                { return edge.From.MapId == from.MapId && Distance(edge.From, from) <= 2 && Distance(edge.To, to) <= 1; });
            return corridor || !Revisited(to) || CanBacktrack(from, to);
        }
        void CommitRejoin(ActionPosition const& from, ActionPosition const& to)
        {
            if (Allows(from, to, false, true)) Rejoins.push_back({from, to});
        }
        void PlanContinuation(ActionPosition const& connector, std::vector<ActionPosition> continuation, bool surface)
        {
            Planned = std::move(continuation);
            SurfaceCorridor = surface;
            // The connector is not reached when its continuation is installed.
            // Keep it as a guard so a nearby later corner cannot be consumed
            // while the actor is still approaching this route's real start.
            if (surface && !Planned.empty() && Distance(Planned.front(), connector) > 0.01f)
                Planned.insert(Planned.begin(), connector);
        }
        bool PlanRejoin(ActionPosition const& from, ActionPosition const& to, std::vector<ActionPosition> continuation,
            bool surface = false)
        {
            if (continuation.empty() || !Allows(from, to, false, true)) return false;
            PlanContinuation(to, std::move(continuation), surface);
            return true;
        }

        bool Failed(ActionPosition const& from, ActionPosition const& to) const
        {
            return std::any_of(FailedEdges.begin(), FailedEdges.end(), [&](Backtrack const& edge)
            { return edge.From.MapId == from.MapId && Distance(edge.From, from) <= 2.0f && Distance(edge.To, to) <= 1.0f; });
        }
        void Reject(ActionPosition const& from, ActionPosition const& to)
        {
            MarkIneffective(from, to);
            Planned.clear();
            SurfaceCorridor = false;
        }
        void MarkIneffective(ActionPosition const& from, ActionPosition const& to)
        {
            if (!Finite(from) || !Finite(to) || Failed(from, to)) return;
            if (FailedEdges.size() == MaxTrailPoints) FailedEdges.erase(FailedEdges.begin());
            FailedEdges.push_back({from, to});
        }
        bool Advance(ActionPosition const& here)
        {
            bool advanced = false;
            while (!Planned.empty() && Planned.front().MapId == here.MapId &&
                Distance(Planned.front(), here) <= (SurfaceCorridor ? 0.5f : ArrivalToleranceYards))
            { Planned.erase(Planned.begin()); advanced = true; }
            if (Planned.empty()) SurfaceCorridor = false;
            return advanced;
        }

        bool CanBacktrack(ActionPosition const& from, ActionPosition const& to) const
        {
            // Only a fallback after all new routes fail. Each directed edge
            // once, at most 16 without a new best distance to home. Merely
            // walking sideways or restarting an animation does not reset it.
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
