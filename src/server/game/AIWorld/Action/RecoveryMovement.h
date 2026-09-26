/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_RECOVERYMOVEMENT_H
#define AIWORLD_RECOVERYMOVEMENT_H
#include "ActionPosition.h"
#include <optional>

struct RecoveryMovement
{
    ActionPosition Destination, Home;
    float HomeRadius = 0.0f;
    std::optional<ActionPosition> Danger;
    bool Rejoin = false;
    // Preserve the query endpoint when the planner normalizes its last point.
    std::optional<ActionPosition> QueryDestination;

    static bool SamePoint(ActionPosition const& a, ActionPosition const& b)
    { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; }

    bool operator==(RecoveryMovement const& other) const
    {
        return SamePoint(Destination, other.Destination) && SamePoint(Home, other.Home) &&
            HomeRadius == other.HomeRadius && Rejoin == other.Rejoin &&
            Danger.has_value() == other.Danger.has_value() && (!Danger || SamePoint(*Danger, *other.Danger)) &&
            QueryDestination.has_value() == other.QueryDestination.has_value() &&
            (!QueryDestination || SamePoint(*QueryDestination, *other.QueryDestination));
    }
};
#endif
