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
    };

    inline bool Finite(ActionPosition const& point)
    {
        return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
    }

    inline float Distance(ActionPosition const& a, ActionPosition const& b)
    {
        return std::hypot(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    }

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
