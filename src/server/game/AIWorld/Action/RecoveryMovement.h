/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_RECOVERYMOVEMENT_H
#define AIWORLD_RECOVERYMOVEMENT_H
#include "ActionPosition.h"
#include "ObjectGuid.h"
#include <algorithm>
#include <optional>
#include <vector>

struct RecoveryMovement
{
    ActionPosition Destination, Home;
    float HomeRadius = 0.0f;
    std::optional<ActionPosition> Danger;
    bool Rejoin = false;
    // Explicit opt-in for a fully validated terrain route across a navmesh
    // gap. Each short leg must still be checked again before execution.
    bool SurfaceCorridor = false;
    // Preserve the query endpoint when the planner normalizes its last point.
    std::optional<ActionPosition> QueryDestination;
    float DangerRadius = 8.0f;
    // Only an explicitly owned, completely planned resident surface route
    // may carry short corners as one bounded linear spline. This is never a
    // request to replace those corners with a chord to Destination.
    struct SurfacePolyline
    {
        ObjectGuid Owner;
        uint32 PhaseMask = 0;
        ActionPosition Goal;
        std::vector<ActionPosition> Points; // original physical source first
        std::vector<ActionPosition> SplinePoints; // exact validated dense launch controls
        bool operator==(SurfacePolyline const& other) const
        {
            return Owner == other.Owner && PhaseMask == other.PhaseMask && SamePoint(Goal,other.Goal) &&
                Points.size() == other.Points.size() &&
                std::equal(Points.begin(),Points.end(),other.Points.begin(),SamePoint) &&
                SplinePoints.size() == other.SplinePoints.size() &&
                std::equal(SplinePoints.begin(),SplinePoints.end(),other.SplinePoints.begin(),SamePoint);
        }
    };
    std::optional<SurfacePolyline> Polyline;

    static bool SamePoint(ActionPosition const& a, ActionPosition const& b)
    { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; }

    bool operator==(RecoveryMovement const& other) const
    {
        return SamePoint(Destination, other.Destination) && SamePoint(Home, other.Home) &&
            HomeRadius == other.HomeRadius && Rejoin == other.Rejoin && SurfaceCorridor == other.SurfaceCorridor &&
            DangerRadius == other.DangerRadius &&
            Danger.has_value() == other.Danger.has_value() && (!Danger || SamePoint(*Danger, *other.Danger)) &&
            QueryDestination.has_value() == other.QueryDestination.has_value() &&
            (!QueryDestination || SamePoint(*QueryDestination, *other.QueryDestination)) &&
            Polyline.has_value() == other.Polyline.has_value() && (!Polyline || *Polyline == *other.Polyline);
    }
};
#endif
