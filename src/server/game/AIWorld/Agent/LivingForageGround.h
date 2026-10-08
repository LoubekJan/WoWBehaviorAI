/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGFORAGEGROUND_H
#define AIWORLD_LIVINGFORAGEGROUND_H
#include "LivingSurfaceCorridor.h"

// Endpoint resolution only. One admitted call performs at most eight height
// queries and eight segment checks. A complete navmesh/execution proof is a
// separate planning operation; an unsupported floor never grants movement.
struct LivingForageGroundSearch
{
    using Status = LivingSurfaceCorridor::Status;
    bool Started = false;
    ActionPosition Origin, Target;
    Status State = Status::Pending;
    std::optional<ActionPosition> Resolved;
    LivingSurfaceCorridor::Search Surface;

    template<class HeightAt, class ClearSegment>
    Status Advance(ActionPosition const& from, ActionPosition const& target,
        HeightAt&& heightAt, ClearSegment&& clearSegment)
    {
        auto same = [](ActionPosition const& a, ActionPosition const& b)
        { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; };
        auto reject = [&]() { Resolved.reset(); State = Status::Rejected; return State; };
        if (Started && (!same(Origin, from) || !same(Target, target))) return reject();
        if (State != Status::Pending) return State;
        unsigned samples = 8;
        if (!Started)
        {
            Started = true; Origin = from; Target = target;
            if (!LivingSurfaceCorridor::Finite(from) || !LivingSurfaceCorridor::Finite(target) ||
                from.MapId != target.MapId || std::hypot(target.X-from.X, target.Y-from.Y) > 16.01f)
                return reject();
            auto point = target; point.Z = from.Z;
            auto height = heightAt(point);
            if (height && std::isfinite(*height) && std::abs(*height-from.Z) <= 3.0f)
            { point.Z = *height; Resolved = point; State = Status::Complete; return State; }
            // The endpoint probe and the surface origin consume two of this
            // call's eight height queries. Later calls resume only new samples.
            samples = 6;
        }
        State = LivingSurfaceCorridor::Advance(Surface, from, target, heightAt, clearSegment,
            [](ActionPosition const&) { return true; }, samples, LivingSurfaceCorridor::InstalledTiles{}, true);
        if (State == Status::Complete) Resolved = Surface.Route.back();
        return State;
    }
};
#endif
