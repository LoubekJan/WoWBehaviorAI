/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "LivingRecoveryPath.h"
#include "Agent/LivingRolePolicy.h"
#include "Creature.h"
#include "ElwynnHuntPath.h"
#include "GroundedPathSupport.h"
#include "Map.h"
#include "MovementPathBounds.h"
#include "PathGenerator.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <utility>

namespace LivingRecoveryPath
{
    namespace
    {
        std::optional<float> TerrainHeight(Creature const& creature, ActionPosition const& point, float lift = 0.3f)
        {
            // Query just above the expected feet, not GetMapHeight's two-yard
            // lift: that lift can select a nearby ledge or the floor above.
            float hover = creature.GetHoverOffset();
            float height = creature.GetMap()->GetHeight(creature.GetPhaseMask(), point.X, point.Y,
                point.Z-hover+lift, true, 3.0f);
            if (!std::isfinite(height) || height <= INVALID_HEIGHT) return std::nullopt;
            return height + hover;
        }
        bool ClearGroundSegment(Creature const& creature, ActionPosition const& a, ActionPosition const& b)
        {
            for (float lift : {0.3f, std::max(0.5f, creature.GetCollisionHeight())})
                if (!creature.GetMap()->isInLineOfSight(a.X, a.Y, a.Z+lift, b.X, b.Y, b.Z+lift,
                    creature.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)) return false;
            return true;
        }
        bool ClearSurfaceBody(Creature const& creature, ActionPosition const& a, ActionPosition const& b)
        {
            return LivingSurfaceCorridor::BodyClear(a, b, creature.GetBoundingRadius(), creature.GetCollisionHeight(),
                [&](ActionPosition const& from, ActionPosition const& to)
                { return creature.GetMap()->isInLineOfSight(from.X, from.Y, from.Z, to.X, to.Y, to.Z,
                    creature.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing); });
        }
    }

    std::optional<ActionPosition> GroundHomeTarget(Creature& creature, ActionPosition const& home,
        ActionPosition const& target)
    {
        if (home.MapId != creature.GetMapId()) return std::nullopt;
        return LivingReturnPolicy::GroundHomeTarget(home, target,
            [&](ActionPosition const& p) -> std::optional<float>
            { return TerrainHeight(creature, p, LivingReturnPolicy::SamePosition(p, home) ? 0.3f : 0.8f); },
            [&](ActionPosition const& a, ActionPosition const& b)
            {
                return ClearGroundSegment(creature, a, b);
            });
    }
    LivingSurfaceCorridor::Status GroundLocalForageTarget(Creature& creature, ActionPosition const& from,
        ActionPosition const& target, LivingForageGroundSearch& search, bool retainSupportedPrefix)
    {
        return search.Advance(from, target,
            [&](ActionPosition const& p)
            { return TerrainHeight(creature, p, LivingReturnPolicy::SamePosition(p, from) ? 0.3f : 0.8f); },
            [&](ActionPosition const& a, ActionPosition const& b)
            { return retainSupportedPrefix ? ClearSurfaceBody(creature, a, b) : ClearGroundSegment(creature, a, b); },
            retainSupportedPrefix);
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

    std::vector<ActionPosition> HomeCorridor(Creature& creature, ActionPosition const& from,
        ActionPosition const& home, float arrivalRadius, float limit, ActionPosition const* danger, float clearance,
        LivingReturnPolicy::Diagnostics* diagnostics, LivingReturnPolicy::HomeCorridorSearch* search, bool allowDetour)
    {
        using namespace LivingReturnPolicy;
        HomeCorridorSearch localSearch;
        auto& cursor = search ? *search : localSearch;
        if (!cursor.Matches(from, home, arrivalRadius, limit, danger, clearance, allowDetour))
            cursor.Begin(from, home, arrivalRadius, limit, danger, clearance, allowDetour);
        auto& report = cursor.Report;
        if (diagnostics) { diagnostics->Deferred = false; diagnostics->HomePath = report; }
        auto publish = [&] { if (diagnostics) diagnostics->HomePath = report; };
        if (cursor.Done) return {};
        if (!Finite(from) || !Finite(home) || from.MapId != 0 || home.MapId != 0 || creature.GetMapId() != 0 ||
            !std::isfinite(limit) || limit <= 0 || !std::isfinite(arrivalRadius) || arrivalRadius < 0 ||
            !std::isfinite(clearance) || clearance < 0 || clearance > 30 ||
            (danger && (!Finite(*danger) || danger->MapId != from.MapId)))
        { cursor.Done = true; publish(); return {}; }
        if (!cursor.ArrivalChecked)
        {
            if (std::hypot(from.X-home.X, from.Y-home.Y) <= arrivalRadius)
            {
                std::optional<PlanningWorkBudget::Permit> permit;
                if (search) permit.emplace(PlanningWorkBudget::TryAcquire());
                if (permit && !*permit)
                { if (diagnostics) diagnostics->Deferred = true; publish(); return {}; }
                if (HomeEndpointMatches(home, arrivalRadius, from, GroundHomeTarget(creature, home, from)))
                { report.Failure = "NONE"; cursor.Done = true; publish(); return {from}; }
            }
            cursor.ArrivalChecked = true;
        }
        auto targets = HomeTargets(home, arrivalRadius);
        for (; cursor.NextTarget < targets.size(); ++cursor.NextTarget)
        {
            std::optional<PlanningWorkBudget::Permit> permit;
            if (search) permit.emplace(PlanningWorkBudget::TryAcquire());
            if (permit && !*permit)
            { if (diagnostics) diagnostics->Deferred = true; publish(); return {}; }
            auto target = targets[cursor.NextTarget];
            auto ground = GroundHomeTarget(creature, home, target);
            if (!ground) { report.Reject(HomePathFailure::Ground, 0); continue; }
            target = *ground;
            cursor.GroundTargets[cursor.NextTarget] = target;
            PathGenerator path(&creature);
            path.AllowSteepSlopes();
            bool calculated = path.CalculatePathFrom({from.X, from.Y, from.Z}, {target.X, target.Y, target.Z});
            auto const& navigation = path.GetNavigationDiagnostics();
            cursor.SurfaceEligible[cursor.NextTarget] = navigation.Mesh && navigation.StartTile && navigation.EndTile;
            auto reject = [&](HomePathFailure failure) { report.Reject(failure, uint32(path.GetPathType())); };
            if (!calculated || !Movement::CompleteNavmeshPath(path.GetPathType()))
            { reject(HomePathFailure::NoPath); continue; }
            auto points = path.GetPath();
            if (points.size() < 2 || Distance(from, {0, points.front().x, points.front().y, points.front().z}) > 1.5f ||
                std::hypot(points.back().x-home.X, points.back().y-home.Y) > arrivalRadius ||
                !HomeEndpointMatches(home, arrivalRadius, {0, points.back().x, points.back().y, points.back().z},
                    GroundHomeTarget(creature, home, {0, points.back().x, points.back().y, points.back().z})))
            { reject(HomePathFailure::Endpoint); continue; }
            points.front() = {from.X, from.Y, from.Z};
            if (!Movement::PathWithinBounds(points, [&](float x, float y, float z)
                { return creature.GetMap()->GetZoneId(creature.GetPhaseMask(), x, y, z) == 12 &&
                    std::hypot(x-home.X, y-home.Y) <= limit; }))
            { reject(HomePathFailure::Bounds); continue; }
            bool safe = true;
            std::vector<ActionPosition> route{from};
            for (auto const& point : points)
            {
                if (danger && !LivingRolePolicy::AvoidsDanger(route.back().X, route.back().Y,
                    point.x, point.y, danger->X, danger->Y, clearance)) { safe = false; break; }
                route.push_back({0, point.x, point.y, point.z});
            }
            if (!safe) { reject(HomePathFailure::Danger); continue; }
            auto corridor = Corridor(route);
            if (corridor.empty()) { reject(HomePathFailure::Corridor); continue; }
            report.PathType = uint32(path.GetPathType());
            report.Failure = "NONE";
            cursor.IssueCorridor(false);
            publish();
            return corridor;
        }
        // A navmesh hole is not evidence of a physical wall. Plan the whole
        // alternative on supported terrain before accepting any short leg.
        // The explicit marker keeps this separate from a complete navmesh
        // result, and the movement provider revalidates each launched leg.
        PathGenerator surfaceTiles(&creature);
        while (cursor.NextSurfaceTarget < targets.size())
        {
            auto index = cursor.NextSurfaceTarget;
            if (!cursor.SurfaceEligible[index] || !cursor.GroundTargets[index])
            { ++cursor.NextSurfaceTarget; continue; }
            std::optional<PlanningWorkBudget::Permit> permit;
            if (search) permit.emplace(PlanningWorkBudget::TryAcquire());
            if (permit && !*permit)
            { if (diagnostics) diagnostics->Deferred = true; publish(); return {}; }
            if (!cursor.Surface.Started)
                cursor.Surface.Target = *cursor.GroundTargets[index];
            auto status = LivingSurfaceCorridor::Advance(cursor.Surface, from, cursor.Surface.Target,
                [&](ActionPosition const& p)
                { return TerrainHeight(creature, p, SamePosition(p, from) ? 0.3f : 0.8f); },
                [&](ActionPosition const& a, ActionPosition const& b)
                {
                    return (!danger || LivingRolePolicy::AvoidsDanger(a.X, a.Y, b.X, b.Y,
                        danger->X, danger->Y, clearance)) && ClearSurfaceBody(creature, a, b);
                },
                [&](ActionPosition const& p)
                { return creature.GetMap()->GetZoneId(creature.GetPhaseMask(), p.X, p.Y, p.Z) == 12 &&
                    std::hypot(p.X-home.X, p.Y-home.Y) <= limit; }, 8,
                [&](ActionPosition const& p) { return surfaceTiles.RecoveryTile({p.X, p.Y, p.Z}); });
            report.SurfaceFailure = cursor.Surface.Failure;
            if (status == LivingSurfaceCorridor::Status::Pending)
                continue;
            if (status == LivingSurfaceCorridor::Status::Complete &&
                HomeEndpointMatches(home, arrivalRadius, cursor.Surface.Route.back(),
                    GroundHomeTarget(creature, home, cursor.Surface.Route.back())))
            {
                auto corridor = LivingSurfaceCorridor::Legs(cursor.Surface.Route);
                if (!corridor.empty())
                {
                    report.SurfaceCorridor = true; report.Failure = "NONE";
                    cursor.IssueCorridor(true); publish(); return corridor;
                }
            }
            if (status == LivingSurfaceCorridor::Status::Complete)
                report.SurfaceFailure = "SURFACE_ENDPOINT_MISMATCH";
            cursor.Surface = {};
            ++cursor.NextSurfaceTarget;
        }
        if (allowDetour) return SurfaceDetour(creature, cursor, diagnostics);
        cursor.Done = true;
        publish();
        return {};
    }

    std::vector<ActionPosition> SurfaceDetour(Creature& creature,
        LivingReturnPolicy::HomeCorridorSearch& cursor, LivingReturnPolicy::Diagnostics* diagnostics)
    {
        using namespace LivingReturnPolicy;
        auto& report = cursor.Report;
        if (diagnostics) { diagnostics->Deferred = false; diagnostics->HomePath = report; }
        auto publish = [&] { if (diagnostics) diagnostics->HomePath = report; };
        auto const& from = cursor.From;
        auto const& home = cursor.Home;
        auto const* danger = cursor.Danger ? &*cursor.Danger : nullptr;
        // A graph never repeats the cheap query stages, and cannot use tile
        // eligibility obtained at another actor origin or geometric request.
        if (!cursor.HasContext || !Finite(from) || !Finite(home) || from.MapId != 0 || home.MapId != 0 ||
            creature.GetMapId() != from.MapId || !creature.CanWalk() ||
            !SamePosition(from, {creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}) ||
            !cursor.CandidatesExhausted(HomeTargets(home, cursor.ArrivalRadius).size()) ||
            !std::any_of(cursor.SurfaceEligible.begin(), cursor.SurfaceEligible.end(), [](bool eligible) { return eligible; }))
        { cursor.Done = true; publish(); return {}; }
        PathGenerator surfaceTiles(&creature);
        while (cursor.Detour.State == LivingSurfaceCorridor::Status::Pending)
        {
            auto permit = PlanningWorkBudget::TryAcquire();
            if (!permit)
            { if (diagnostics) diagnostics->Deferred = true; publish(); return {}; }
            LivingSurfaceDetour::Advance(cursor.Detour, from, home, cursor.ArrivalRadius, cursor.Limit,
                [&](ActionPosition const& p)
                { return TerrainHeight(creature, p, SamePosition(p, from) ? 0.3f : 0.8f); },
                [&](ActionPosition const& a, ActionPosition const& b)
                { return (!danger || LivingRolePolicy::AvoidsDanger(a.X, a.Y, b.X, b.Y,
                    danger->X, danger->Y, cursor.Clearance)) && ClearSurfaceBody(creature, a, b); },
                [&](ActionPosition const& p)
                { return creature.GetMap()->GetZoneId(creature.GetPhaseMask(), p.X, p.Y, p.Z) == 12 &&
                    std::hypot(p.X-home.X, p.Y-home.Y) <= cursor.Limit; },
                [&](ActionPosition const& p)
                { return HomeEndpointMatches(home, cursor.ArrivalRadius, p, GroundHomeTarget(creature, home, p)); },
                8, 1, [&](ActionPosition const& p) { return surfaceTiles.RecoveryTile({p.X, p.Y, p.Z}); });
            report.DetourFailure = cursor.Detour.Failure;
            report.DetourNodes = uint32(cursor.Detour.Nodes.size());
            report.DetourEdges = cursor.Detour.EdgeAttempts;
        }
        cursor.Done = true;
        if (cursor.Detour.State == LivingSurfaceCorridor::Status::Complete && !cursor.Detour.Route.empty())
        {
            report.SurfaceCorridor = true; report.Failure = "NONE";
            publish();
            // Publish a complete proof once. If its first execution leg fails,
            // fallback orchestration cannot reinstall the same rejected route.
            return cursor.TakeDetourRoute();
        }
        publish();
        return {};
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
        return ActionPosition{creature.GetMapId(), point.x, point.y, point.z};
    }

    std::vector<ActionPosition> RejoinPositions(Creature& creature, NavigationDiagnostics* diagnostics,
        LivingReturnPolicy::RejoinSearch* search, bool* deferred)
    {
        LivingReturnPolicy::RejoinSearch localSearch;
        auto& cursor = search ? *search : localSearch;
        ActionPosition from{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        if (!cursor.HasContext || !LivingReturnPolicy::SamePosition(cursor.From, from))
        { cursor = {}; cursor.HasContext = true; cursor.From = from; }
        if (deferred) *deferred = false;
        auto publish = [&] { if (diagnostics) *diagnostics = cursor.Navigation; };
        // A resumed continuation has more recent connector diagnostics. Do
        // not overwrite them with the final probe merely to reuse candidates.
        if (cursor.Done) return cursor.Candidates;
        PathGenerator query(&creature);
        query.AllowSteepSlopes();
        // Nearest plus three rings: the old single four-yard ring could miss
        // the valid edge inside the six-yard physical connector boundary.
        for (; cursor.NextProbe < 25; ++cursor.NextProbe)
        {
            std::optional<PlanningWorkBudget::Permit> permit;
            if (search) permit.emplace(PlanningWorkBudget::TryAcquire());
            if (permit && !*permit)
            { if (deferred) *deferred = true; publish(); return {}; }
            G3D::Vector3 probe(from.X, from.Y, from.Z), point;
            if (cursor.NextProbe)
            {
                auto ring = (cursor.NextProbe-1) / 8;
                float radius = ring == 0 ? 2.0f : ring == 1 ? 4.0f : 5.5f;
                float angle = float((cursor.NextProbe-1) % 8) * 0.78539816f;
                probe.x += radius*std::cos(angle); probe.y += radius*std::sin(angle);
            }
            if (query.FindRecoveryPosition(point, cursor.NextProbe ? &probe : nullptr, &cursor.Navigation))
            {
                ActionPosition candidate{creature.GetMapId(), point.x, point.y, point.z};
                if (std::none_of(cursor.Candidates.begin(), cursor.Candidates.end(), [&](auto const& p)
                    { return LivingReturnPolicy::Distance(p, candidate) < 1; })) cursor.Candidates.push_back(candidate);
            }
        }
        cursor.Done = true; publish(); return cursor.Candidates;
    }

    bool Build(Creature& creature, RecoveryMovement const& request, Movement::PointsArray& points,
        LivingReturnPolicy::Diagnostics* diagnostics)
    {
        using namespace LivingReturnPolicy;
        points.clear();
        G3D::Vector3 source(creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ());
        bool sourceReady = Movement::PhysicalSplineSource(creature, source);
        ActionPosition from{creature.GetMapId(), source.x, source.y, source.z};
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
        if (!sourceReady) return reject("INVALID_MOVEMENT_SOURCE");
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
            if (request.SurfaceCorridor) return ClearSurfaceBody(creature, a, b);
            // Check foot clearance as well as body height (small fences matter).
            for (float lift : {0.3f, std::max(0.5f, creature.GetCollisionHeight())})
                if (!map->isInLineOfSight(a.X, a.Y, a.Z + lift, b.X, b.Y, b.Z + lift,
                    creature.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)) return false;
            return true;
        };
        PathGenerator path(&creature);
        path.AllowSteepSlopes();
        bool calculated = !request.SurfaceCorridor && path.CalculatePathFrom(source, {to.X, to.Y, to.Z});
        nav = request.SurfaceCorridor ? path.RecoveryTiles({from.X, from.Y, from.Z}, {to.X, to.Y, to.Z}) :
            path.GetNavigationDiagnostics();
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
        if (request.SurfaceCorridor)
        {
            if (!creature.CanWalk() || Distance(from, to) > 6.0f) return reject("SURFACE_RANGE");
            LivingSurfaceCorridor::Search surface;
            auto status = LivingSurfaceCorridor::Advance(surface, from, to,
                [&](ActionPosition const& p)
                { return TerrainHeight(creature, p, SamePosition(p, from) ? 0.3f : 0.8f); }, clearSegment,
                [&](ActionPosition const& p)
                { return map->GetZoneId(creature.GetPhaseMask(), p.X, p.Y, p.Z) == 12 &&
                    std::hypot(p.X-request.Home.X, p.Y-request.Home.Y) <= request.HomeRadius; }, 12,
                [&](ActionPosition const& p) { return path.RecoveryTile({p.X, p.Y, p.Z}); });
            nav.Detail = surface.Failure;
            if (status != LivingSurfaceCorridor::Status::Complete) return reject(surface.Failure);
            for (auto const& p : surface.Route) points.emplace_back(p.X, p.Y, p.Z);
        }
        else if (request.Rejoin)
        {
            G3D::Vector3 probe(to.X, to.Y, to.Z), projected;
            if (!creature.CanWalk() || !path.FindRecoveryPosition(projected, &probe, &nav)) return reject("REJOIN_CHANGED");
            if (Distance({from.MapId, projected.x, projected.y, projected.z}, to) > 1.0f) return reject("REJOIN_CHANGED");
            char const* connectorFailure = "NONE";
            std::optional<ActionPosition> support;
            G3D::Vector3 startProjection;
            if (path.FindRecoveryPosition(startProjection, &source, &nav))
            {
                support = ActionPosition{from.MapId, startProjection.x, startProjection.y, startProjection.z};
            }
            auto connector = SurfaceConnector(from, to, [&](ActionPosition const& p) -> std::optional<float>
            {
                auto height = TerrainHeight(creature, p);
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
            {
                auto raw = path.GetPath();
                G3D::Vector3 origin(from.X, from.Y, from.Z);
                raw.front() = origin;
                bool swimming = creature.CanEnterWater() && Movement::FullySwimmingPath(raw,
                    [&](G3D::Vector3 const& p)
                    { return InSwimmableWater(creature, {from.MapId, p.x, p.y, p.z}); });
                if (creature.IsFlying() || swimming)
                {
                    points = std::move(raw);
                    nav.Swimming = swimming;
                }
                else
                {
                    // A complete mesh leg can still carry elevated vertices.
                    // Launch dense controls on the physical floor so stopping
                    // between vertices cannot leave the actor floating above it.
                    char const* groundFailure = "NONE";
                    bool grounded = Movement::PrepareGroundedPath(raw, origin, points,
                        [&](G3D::Vector3 const& p)
                        { return TerrainHeight(creature, {from.MapId, p.x, p.y, p.z},
                            p.x == origin.x && p.y == origin.y && p.z == origin.z ? 0.3f : 0.8f); },
                        [&](G3D::Vector3 const& a, G3D::Vector3 const& b)
                        {
                            if (ClearSurfaceBody(creature, {from.MapId, a.x, a.y, a.z},
                                {from.MapId, b.x, b.y, b.z})) return true;
                            nav.RejectedX = b.x; nav.RejectedY = b.y; nav.RejectedZ = b.z;
                            return false;
                        },
                        [&](G3D::Vector3 const& p)
                        { return map->GetZoneId(creature.GetPhaseMask(), p.x, p.y, p.z) == 12 &&
                            std::hypot(p.x-request.Home.X, p.y-request.Home.Y) <= request.HomeRadius; },
                        [&](G3D::Vector3 const& p) { return path.RecoveryTile(p); }, &groundFailure);
                    nav.Detail = groundFailure;
                    if (!grounded) return reject("UNSAFE_GROUND_PATH");
                }
            }
        }
        // Swimming capability comes from the movement template too. The native
        // CAN_SWIM flag may disappear after evade. Validate EVERY water segment
        // here instead of accepting the engine's endpoint-only water shortcut.
        if (!request.Rejoin && !request.SurfaceCorridor && points.empty() && creature.CanEnterWater())
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
