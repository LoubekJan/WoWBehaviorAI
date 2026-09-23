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
#include <cmath>
#include <optional>
#include <vector>

namespace LivingReturnPolicy
{
    // Stay below the 30-yard path gate. At Elwynn coordinates, a nominal
    // 30-yard float step can round outward and fail that gate forever.
    constexpr float MaxStepLength = 28.0f;

    // Follow the route, rather than projecting a straight chord toward home.
    // The engine still checks the selected step's ground, zone, LOS and path.
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
