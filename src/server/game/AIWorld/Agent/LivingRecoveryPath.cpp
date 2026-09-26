/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "LivingRecoveryPath.h"
#include "Agent/LivingRolePolicy.h"
#include "Creature.h"
#include "Map.h"
#include "MovementPathBounds.h"
#include "PathGenerator.h"

namespace LivingRecoveryPath
{
    bool InSwimmableWater(Creature const& creature, ActionPosition const& point)
    {
        if (!creature.CanEnterWater() || point.MapId != creature.GetMapId()) return false;
        auto liquid = creature.GetMap()->GetLiquidStatus(creature.GetPhaseMask(), point.X, point.Y, point.Z,
            {}, nullptr, creature.GetCollisionHeight());
        // WATER_WALK is the engine's classification for a point within 0.1 yd
        // of the water surface, also a valid swimming endpoint (not dry air).
        return (liquid & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)) != 0;
    }

    std::optional<ActionPosition> RejoinPosition(Creature& creature)
    {
        PathGenerator query(&creature);
        query.AllowSteepSlopes();
        G3D::Vector3 point;
        if (!query.FindRecoveryPosition(point)) return std::nullopt;
        return ActionPosition{creature.GetMapId(), point.x, point.y, point.z};
    }

    std::vector<ActionPosition> RejoinPositions(Creature& creature)
    {
        std::vector<ActionPosition> result;
        if (auto nearest = RejoinPosition(creature)) result.push_back(*nearest);
        PathGenerator query(&creature);
        query.AllowSteepSlopes();
        for (unsigned i = 0; i < 8; ++i)
        {
            float angle = float(i) * 0.78539816f;
            G3D::Vector3 probe(creature.GetPositionX()+4*std::cos(angle),
                creature.GetPositionY()+4*std::sin(angle), creature.GetPositionZ()), point;
            if (query.FindRecoveryPosition(point, &probe))
            {
                ActionPosition candidate{creature.GetMapId(), point.x, point.y, point.z};
                if (std::none_of(result.begin(), result.end(), [&](auto const& p)
                    { return LivingReturnPolicy::Distance(p, candidate) < 1; })) result.push_back(candidate);
            }
        }
        return result;
    }

    bool Build(Creature& creature, RecoveryMovement const& request, Movement::PointsArray& points,
        LivingReturnPolicy::Diagnostics* diagnostics)
    {
        using namespace LivingReturnPolicy;
        points.clear();
        ActionPosition from{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        auto const& to = request.QueryDestination.value_or(request.Destination);
        NavigationDiagnostics nav;
        nav.HomeRadius = request.HomeRadius;
        auto reject = [&](char const* reason)
        {
            nav.Failure = reason;
            if (diagnostics) diagnostics->Navigation = nav;
            points.clear();
            return false;
        };
        if (from.MapId != 0 || creature.GetZoneId() != 12 || creature.IsInCombat() || creature.IsInEvadeMode() ||
            creature.IsPet() || creature.IsCharmed() || !creature.GetTransGUID().IsEmpty() ||
            !creature.IsAlive() || !UsefulStep(from, to) || !UsefulStep(from, request.Destination) || Distance(from, to) > 30.0f ||
            !Finite(request.Home) || request.Home.MapId != from.MapId ||
            !std::isfinite(request.HomeRadius) || request.HomeRadius <= 0.0f ||
            (request.Danger && (!Finite(*request.Danger) || request.Danger->MapId != from.MapId))) return reject("INVALID_REQUEST");
        Map* map = creature.GetMap();
        auto clearSegment = [&](ActionPosition const& a, ActionPosition const& b)
        {
            // Check foot clearance as well as body height (small fences matter).
            for (float lift : {0.3f, std::max(0.5f, creature.GetCollisionHeight())})
                if (!map->isInLineOfSight(a.X, a.Y, a.Z + lift, b.X, b.Y, b.Z + lift,
                    creature.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)) return false;
            return true;
        };
        PathGenerator path(&creature);
        path.AllowSteepSlopes();
        bool calculated = path.CalculatePath(to.X, to.Y, to.Z, false);
        nav = path.GetNavigationDiagnostics();
        nav.HomeRadius = request.HomeRadius;
        if (diagnostics)
        {
            diagnostics->PathType = uint32(path.GetPathType());
            diagnostics->Navigation = nav;
        }
        // Missing tiles are an installation problem, not permission to cross
        // arbitrary geometry with the engine's no-mmap straight-line fallback.
        if (!nav.Mesh || !nav.StartTile || !nav.EndTile) return reject("MISSING_NAVMESH_TILE");
        if (request.Rejoin)
        {
            G3D::Vector3 probe(to.X, to.Y, to.Z), projected;
            if (!creature.CanWalk() || !path.FindRecoveryPosition(projected, &probe) ||
                Distance({from.MapId, projected.x, projected.y, projected.z}, to) > 1.0f) return reject("REJOIN_CHANGED");
            char const* connectorFailure = "NONE";
            auto connector = SurfaceConnector(from, to, [&](ActionPosition const& p) -> std::optional<float>
            {
                float height = map->GetHeight(creature.GetPhaseMask(), p.X, p.Y, p.Z + 1.0f, true);
                if (!std::isfinite(height) || height <= INVALID_HEIGHT) return std::nullopt;
                return height;
            }, clearSegment, &connectorFailure);
            nav.Detail = connectorFailure;
            for (auto const& p : connector) points.emplace_back(p.X, p.Y, p.Z);
            nav.Rejoin = !points.empty();
        }
        else if (calculated && (path.GetPathType() & PATHFIND_NORMAL) &&
            !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT |
                PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY)) && path.GetPath().size() >= 2)
        {
            auto const& first = path.GetPath().front();
            if (Distance(from, {from.MapId, first.x, first.y, first.z}) <= 1.5f)
                points = path.GetPath();
        }
        // Swimming capability comes from the movement template too. The native
        // CAN_SWIM flag may disappear after evade. Validate EVERY water segment
        // here instead of accepting the engine's endpoint-only water shortcut.
        if (!request.Rejoin && points.empty() && creature.CanEnterWater())
        {
            auto connector = WaterConnector(from, to, creature.CanEnterWater(), [&](ActionPosition const& p)
            {
                return InSwimmableWater(creature, p);
            }, clearSegment);
            for (auto const& p : connector) points.emplace_back(p.X, p.Y, p.Z);
            nav.Swimming = !points.empty();
        }
        if (diagnostics) diagnostics->Navigation = nav;
        if (points.size() < 2) return reject(request.Rejoin ? "REJOIN_UNSAFE_SURFACE" : "NO_COMPLETE_PATH");
        // Include the real start: MoveSplineInit replaces the first point with
        // the live position. Bounds must cover that exact executed segment too.
        points.front() = G3D::Vector3(from.X, from.Y, from.Z);
        if (!Movement::PathWithinBounds(points, [&](float x, float y, float z)
            {
                bool zone = map->GetZoneId(creature.GetPhaseMask(), x, y, z) == 12;
                bool radius = std::hypot(x - request.Home.X, y - request.Home.Y) <= request.HomeRadius;
                if (!zone || !radius)
                {
                    nav.Detail = !zone ? "ZONE_BOUNDARY" : "HOME_RADIUS";
                    nav.RejectedX = x; nav.RejectedY = y; nav.RejectedZ = z;
                }
                return zone && radius;
            }))
        {
            if (nav.Detail == "NONE") nav.Detail = "GEOMETRY_LIMIT";
            return reject("PATH_BOUNDS");
        }
        auto const& end = points.back();
        if (std::hypot(end.x - to.X, end.y - to.Y) > 1.0f || std::abs(end.z - to.Z) > 1.0f) return reject("ENDPOINT_MISMATCH");
        ActionPosition resolved{from.MapId, end.x, end.y, end.z};
        if (!UsefulStep(from, resolved)) return reject("ZERO_STEP");
        if (request.QueryDestination && Distance(resolved, request.Destination) > 1.0f) return reject("ENDPOINT_CHANGED");
        for (std::size_t i = 1; request.Danger && i < points.size(); ++i)
            if (!LivingRolePolicy::AvoidsDanger(points[i-1].x, points[i-1].y, points[i].x, points[i].y,
                request.Danger->X, request.Danger->Y, 8.0f)) return reject("DANGER_BLOCKED");
        if (diagnostics) diagnostics->Navigation = nav;
        return true;
    }
}
