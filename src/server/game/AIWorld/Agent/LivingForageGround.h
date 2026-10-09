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
    bool Started = false, RetainSupportedPrefix = false;
    ActionPosition Origin, Target;
    Status State = Status::Pending;
    std::optional<ActionPosition> Resolved, Supported;
    LivingSurfaceCorridor::Search Surface;

    template<class HeightAt, class ClearSegment>
    Status Advance(ActionPosition const& from, ActionPosition const& target,
        HeightAt&& heightAt, ClearSegment&& clearSegment, bool retainSupportedPrefix = false)
    {
        auto same = [](ActionPosition const& a, ActionPosition const& b)
        { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y && a.Z == b.Z; };
        auto reject = [&]() { Resolved.reset(); Supported.reset(); State = Status::Rejected; return State; };
        if (Started && (!same(Origin, from) || !same(Target, target) ||
            RetainSupportedPrefix != retainSupportedPrefix)) return reject();
        if (State != Status::Pending) return State;
        unsigned samples = 8;
        if (!Started)
        {
            Started = true; Origin = from; Target = target; RetainSupportedPrefix = retainSupportedPrefix;
            if (!LivingSurfaceCorridor::Finite(from) || !LivingSurfaceCorridor::Finite(target) ||
                from.MapId != target.MapId || std::hypot(target.X-from.X, target.Y-from.Y) > 16.01f)
                return reject();
            if (!retainSupportedPrefix)
            {
                auto point = target; point.Z = from.Z;
                auto height = heightAt(point);
                if (height && std::isfinite(*height) && std::abs(*height-from.Z) <= 3.0f)
                { point.Z = *height; Resolved = point; State = Status::Complete; return State; }
            }
            // A local ray may end beyond a cave corner even when its nearer
            // floor is usable. Resolve that mode from the actor's floor first;
            // a height-valid endpoint on another shelf must not preempt it.
            // Reserve the origin and, if used, endpoint height queries.
            samples = retainSupportedPrefix ? 7 : 6;
        }
        State = LivingSurfaceCorridor::Advance(Surface, from, target, heightAt,
            [&](ActionPosition const& a, ActionPosition const& b)
            {
                if (!clearSegment(a, b)) return false;
                Supported = b;
                return true;
            },
            [](ActionPosition const&) { return true; }, samples, LivingSurfaceCorridor::InstalledTiles{}, true);
        if (State == Status::Complete) Resolved = Surface.Route.back();
        else if (State == Status::Rejected && retainSupportedPrefix && Supported &&
            std::hypot(Supported->X-from.X, Supported->Y-from.Y) > 3.0f)
        {
            // Keep only the continuously grounded prefix, ending before the
            // first failed sample. Three yards is the runtime's reached radius.
            // This is an endpoint proposal: navmesh and execution proofs remain
            // separate, and may reject it for walls, bounds or missing tiles.
            Resolved = Supported;
            State = Status::Complete;
        }
        return State;
    }
};
#endif
