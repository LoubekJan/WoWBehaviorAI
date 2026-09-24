/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#ifndef AIWORLD_LIVINGFORAGEPOLICY_H
#define AIWORLD_LIVINGFORAGEPOLICY_H

#include "Action/ActionPosition.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace LivingForagePolicy
{
    constexpr uint64 DurationMs = 120000;
    constexpr uint64 CooldownMs = 60000;
    constexpr float HomeRadius = 80.0f;

    inline ActionPosition Waypoint(ActionPosition const& home, uint64 id, uint32 leg)
    {
        float angle = float((id % 360 + uint64(leg) * 137) % 360) * 6.28318530718f / 360.0f;
        float radius = 24.0f + 20.0f * (leg % 3);
        return { home.MapId, home.X + radius * std::cos(angle), home.Y + radius * std::sin(angle), home.Z };
    }

    // Only propose a short leg. The runtime still checks ground, zone, LOS,
    // the complete navmesh path and threat memory before authorizing a move.
    inline std::optional<ActionPosition> Step(ActionPosition const& from, ActionPosition const& waypoint, ActionPosition const& home)
    {
        if (from.MapId != home.MapId || waypoint.MapId != home.MapId)
            return std::nullopt;
        float distance = std::hypot(waypoint.X - from.X, waypoint.Y - from.Y);
        if (!std::isfinite(distance) || !std::isfinite(from.Z) || distance <= 2.0f)
            return std::nullopt;
        float fraction = std::min(1.0f, 20.0f / distance);
        ActionPosition result{ from.MapId, from.X + (waypoint.X - from.X) * fraction,
            from.Y + (waypoint.Y - from.Y) * fraction, from.Z };
        float homeDistance = std::hypot(result.X - home.X, result.Y - home.Y);
        if (!std::isfinite(homeDistance) || homeDistance > HomeRadius)
            return std::nullopt;
        return result;
    }
}
#endif
