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
    namespace
    {
        bool NormalizeGround(Creature& creature, G3D::Vector3& point)
        {
            // Match PathGenerator::NormalizePath / the actor's own height
            // query (including its search offset and hover height).
            float ground = creature.GetMapHeight(point.x, point.y, point.z);
            if (!std::isfinite(ground) || ground <= INVALID_HEIGHT || std::abs(ground-point.z) > 3.0f) return false;
            point.z = ground + creature.GetHoverOffset();
            return true;
        }
    }
    std::optional<ActionPosition> Toward(Creature& creature, ActionPosition const& target,
        ActionPosition const& home, float radius, NavigationDiagnostics* diagnostics)
    {
        auto fail = [&](char const* reason) -> std::optional<ActionPosition>
        { if (diagnostics) diagnostics->Failure = reason; return std::nullopt; };
        if (diagnostics) *diagnostics = {};
        // Follow the first corridor corner instead of repeatedly proposing a
        // straight step through the same cave wall. This only proposes a leg;
        // Build revalidates the complete executed leg before it can start.
        if (creature.GetMapId() != 0 || target.MapId != 0 || home.MapId != 0 ||
            !LivingReturnPolicy::Finite(target) || !LivingReturnPolicy::Finite(home) ||
            !std::isfinite(radius) || radius <= 0 || creature.GetExactDist2d(target.X, target.Y) > 256) return fail("INVALID_REQUEST");
        PathGenerator path(&creature);
        path.AllowSteepSlopes();
        bool calculated = path.CalculatePath(target.X, target.Y, target.Z, false);
        if (diagnostics)
        {
            *diagnostics = path.GetNavigationDiagnostics(); diagnostics->HomeRadius = radius;
            diagnostics->SourceX = creature.GetPositionX(); diagnostics->SourceY = creature.GetPositionY();
            diagnostics->SourceZ = creature.GetPositionZ();
        }
        if (!calculated || !(path.GetPathType() & PATHFIND_NORMAL) ||
            (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY |
                PATHFIND_SHORT | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))) return fail("NO_COMPLETE_PATH");
        auto points = path.GetPath();
        if (points.size() < 2 || creature.GetExactDist(points.front().x, points.front().y, points.front().z) > 1.5f ||
            LivingReturnPolicy::Distance(target, {0, points.back().x, points.back().y, points.back().z}) > 1.5f) return fail("ENDPOINT_MISMATCH");
        points.front() = {creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        if (!Movement::PathWithinBounds(points, [&](float x, float y, float z)
            { return creature.GetMap()->GetZoneId(creature.GetPhaseMask(), x, y, z) == 12 &&
                std::hypot(x-home.X, y-home.Y) <= radius; })) return fail("PATH_BOUNDS");
        std::vector<ActionPosition> route;
        for (auto const& p : points) route.push_back({0, p.x, p.y, p.z});
        auto legs = LivingReturnPolicy::Corridor(route);
        for (auto const& leg : legs)
            if (LivingReturnPolicy::UsefulStep(route.front(), leg)) return leg;
        return fail("NO_USEFUL_LEG");
    }

    bool InSwimmableWater(Creature const& creature, ActionPosition const& point)
    {
        if (!creature.CanEnterWater() || point.MapId != creature.GetMapId()) return false;
        auto liquid = creature.GetMap()->GetLiquidStatus(creature.GetPhaseMask(), point.X, point.Y, point.Z,
            {}, nullptr, creature.GetCollisionHeight());
        // WATER_WALK is the engine's classification for a point within 0.1 yd
        // of the water surface, also a valid swimming endpoint (not dry air).
        return (liquid & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)) != 0;
    }

    std::optional<ActionPosition> RejoinPosition(Creature& creature, NavigationDiagnostics* diagnostics)
    {
        PathGenerator query(&creature);
        query.AllowSteepSlopes();
        G3D::Vector3 point;
        if (!query.FindRecoveryPosition(point, nullptr, diagnostics)) return std::nullopt;
        if (!NormalizeGround(creature, point))
        { if (diagnostics) diagnostics->ProjectionFailure = "PROJECTION_GROUND_HEIGHT"; return std::nullopt; }
        if (diagnostics) diagnostics->ProjectionGroundZ = point.z;
        return ActionPosition{creature.GetMapId(), point.x, point.y, point.z};
    }

    std::vector<ActionPosition> RejoinPositions(Creature& creature, NavigationDiagnostics* diagnostics)
    {
        std::vector<ActionPosition> result;
        if (diagnostics) diagnostics->ProjectionProbes = 0;
        if (auto nearest = RejoinPosition(creature, diagnostics)) result.push_back(*nearest);
        PathGenerator query(&creature);
        query.AllowSteepSlopes();
        for (unsigned i = 0; i < 8; ++i)
        {
            float angle = float(i) * 0.78539816f;
            G3D::Vector3 probe(creature.GetPositionX()+4*std::cos(angle),
                creature.GetPositionY()+4*std::sin(angle), creature.GetPositionZ()), point;
            if (query.FindRecoveryPosition(point, &probe, diagnostics))
            {
                if (!NormalizeGround(creature, point))
                { if (diagnostics) diagnostics->ProjectionFailure = "PROJECTION_GROUND_HEIGHT"; continue; }
                if (diagnostics) diagnostics->ProjectionGroundZ = point.z;
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
        nav.SourceX = from.X; nav.SourceY = from.Y; nav.SourceZ = from.Z;
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
            !std::isfinite(request.DangerRadius) || request.DangerRadius < 0.0f || request.DangerRadius > 30.0f ||
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
        nav.SourceX = from.X; nav.SourceY = from.Y; nav.SourceZ = from.Z;
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
            if (!creature.CanWalk() || !path.FindRecoveryPosition(projected, &probe, &nav)) return reject("REJOIN_CHANGED");
            if (!NormalizeGround(creature, projected))
            { nav.ProjectionFailure = "PROJECTION_GROUND_HEIGHT"; return reject("REJOIN_CHANGED"); }
            if (Distance({from.MapId, projected.x, projected.y, projected.z}, to) > 1.0f) return reject("REJOIN_CHANGED");
            char const* connectorFailure = "NONE";
            std::optional<ActionPosition> support;
            G3D::Vector3 startProjection;
            if (path.FindRecoveryPosition(startProjection, nullptr, &nav))
            {
                nav.ProjectionX = startProjection.x; nav.ProjectionY = startProjection.y; nav.ProjectionZ = startProjection.z;
                if (NormalizeGround(creature, startProjection))
                {
                    nav.ProjectionGroundZ = startProjection.z;
                    support = ActionPosition{from.MapId, startProjection.x, startProjection.y, startProjection.z};
                }
                else nav.ProjectionFailure = "PROJECTION_GROUND_HEIGHT";
            }
            auto connector = SurfaceConnector(from, to, [&](ActionPosition const& p) -> std::optional<float>
            {
                float height = creature.GetMapHeight(p.X, p.Y, p.Z);
                if (!std::isfinite(height) || height <= INVALID_HEIGHT) return std::nullopt;
                height += creature.GetHoverOffset();
                if (RecoveryMovement::SamePoint(p, from)) nav.SupportZ = height;
                return height;
            }, clearSegment, &connectorFailure, support, &nav);
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
            if (std::hypot(points[i].x-points[i-1].x, points[i].y-points[i-1].y) > 0.1f &&
                !LivingRolePolicy::AvoidsDanger(points[i-1].x, points[i-1].y, points[i].x, points[i].y,
                request.Danger->X, request.Danger->Y, request.DangerRadius)) return reject("DANGER_BLOCKED");
        if (diagnostics) diagnostics->Navigation = nav;
        return true;
    }
}
