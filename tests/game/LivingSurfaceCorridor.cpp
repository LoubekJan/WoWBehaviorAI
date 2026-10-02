/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingSurfaceCorridor.h"
#include "Agent/LivingReturnPolicy.h"
#include "Agent/LivingRolePolicy.h"
#include "Action/RecoveryMovement.h"
#include "RecoveryProjectionPolicy.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <limits>

namespace
{
    auto flat = [](ActionPosition const&) -> std::optional<float> { return 0.0f; };
    auto clear = [](ActionPosition const&, ActionPosition const&) { return true; };
    auto contains = [](ActionPosition const&) { return true; };
}

TEST_CASE("Recovery navigation validates the entire supported continuation across a navmesh gap", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    Search search;
    ActionPosition from{0,40,0,0}, home{0,0,0,0};
    unsigned heights = 0;
    auto height = [&](ActionPosition const& p) -> std::optional<float> { ++heights; return flat(p); };
    REQUIRE(Advance(search, from, home, height, clear, contains) == Status::Pending);
    CHECK(heights == 9); // one actual start plus one bounded batch
    CHECK(search.NextSample == 9);
    CHECK(search.Route.back().X == 36);
    // The local connector alone cannot prove that home is reachable: a wall
    // beyond the initial batch rejects the whole unpublished route.
    auto wall = [](ActionPosition const& a, ActionPosition const& b) { return !(a.X > 20 && b.X <= 20); };
    while (search.State == Status::Pending) Advance(search, from, home, height, wall, contains);
    REQUIRE(search.State == Status::Rejected);
    CHECK(std::string(search.Failure) == "SURFACE_OBSTACLE");
    CHECK(search.Route.empty());

    Search open;
    while (open.State == Status::Pending) Advance(open, from, home, flat, clear, contains);
    REQUIRE(open.State == Status::Complete);
    REQUIRE_FALSE(Legs(open.Route).empty());
    CHECK(Legs(open.Route).back().X == home.X);
}

TEST_CASE("Recovery navigation resumes surface samples without repeating or publishing a prefix", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    Search search;
    ActionPosition from{0,0,0,0}, home{0,10,0,0};
    std::vector<float> probes;
    auto height = [&](ActionPosition const& p) -> std::optional<float> { probes.push_back(p.X); return 0.0f; };
    REQUIRE(Advance(search, from, home, height, clear, contains, 3) == Status::Pending);
    auto next = search.NextSample;
    auto points = search.Route.size();
    REQUIRE(Advance(search, from, home, height, clear, contains, 0) == Status::Pending);
    CHECK(search.NextSample == next);
    CHECK(search.Route.size() == points);
    while (search.State == Status::Pending) Advance(search, from, home, height, clear, contains, 3);
    REQUIRE(search.State == Status::Complete);
    CHECK(probes.size() == 21);
    for (unsigned i = 0; i < probes.size(); ++i) CHECK(probes[i] == Approx(float(i)*0.5f));
}

TEST_CASE("Recovery navigation surface fallback requires actual start support and rejects another floor", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    Search search;
    // Recorded 79945 actor versus lower-floor height: a nearby polygon is
    // never permission to replace actor Z, settle eight yards or teleport.
    auto lowerFloor = [](ActionPosition const&) -> std::optional<float> { return 77.50289f; };
    REQUIRE(Advance(search, {0,0,0,86.33246f}, {0,4,0,77.50289f}, lowerFloor, clear, contains) == Status::Rejected);
    CHECK(std::string(search.Failure) == "SURFACE_START_HEIGHT");
    CHECK(search.Route.empty());
    Search smallGap;
    REQUIRE(Advance(smallGap, {0,0,0,1.01f}, {0,4,0,0}, flat, clear, contains) == Status::Rejected);
    CHECK(std::string(smallGap.Failure) == "SURFACE_START_HEIGHT");
    Search wrongEnd;
    REQUIRE(Advance(wrongEnd, {0,0,0,0}, {0,4,0,4}, flat, clear, contains) == Status::Rejected);
    CHECK(std::string(wrongEnd.Failure) == "SURFACE_END_HEIGHT");
}

TEST_CASE("Recovery navigation surface fallback rejects walls cliffs holes boundaries and danger", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    ActionPosition from{0,0,0,0}, home{0,4,0,0};
    SECTION("cliff")
    {
        Search s;
        CHECK(Advance(s, from, home, [](ActionPosition const& p) -> std::optional<float>
            { return p.X < 2 ? 0.0f : 2.0f; }, clear, contains) == Status::Rejected);
        CHECK(std::string(s.Failure) == "SURFACE_CLIFF");
    }
    SECTION("missing floor")
    {
        Search s;
        CHECK(Advance(s, from, home, [](ActionPosition const& p) -> std::optional<float>
            { if (p.X == 2) return std::nullopt; return 0.0f; }, clear, contains) == Status::Rejected);
        CHECK(std::string(s.Failure) == "SURFACE_HEIGHT");
    }
    SECTION("foot or body obstacle")
    {
        Search s;
        CHECK(Advance(s, from, home, flat, [](auto const&, auto const& b) { return b.X < 2; }, contains) == Status::Rejected);
        CHECK(std::string(s.Failure) == "SURFACE_OBSTACLE");
    }
    SECTION("invalid middle point despite valid endpoints")
    {
        Search s;
        CHECK(Advance(s, from, home, flat, clear, [](auto const& p) { return p.X < 1.5f || p.X > 2.5f; }) == Status::Rejected);
        CHECK(std::string(s.Failure) == "SURFACE_BOUNDS");
    }
    SECTION("danger between endpoints")
    {
        Search s;
        CHECK(Advance(s, from, home, flat, [](auto const& a, auto const& b)
            { return LivingRolePolicy::AvoidsDanger(a.X,a.Y,b.X,b.Y,2,0,0.4f); }, contains) == Status::Rejected);
        CHECK(std::string(s.Failure) == "SURFACE_OBSTACLE");
    }
    SECTION("map mismatch")
    {
        Search s;
        CHECK(Advance(s, from, {1,4,0,0}, flat, clear, contains) == Status::Rejected);
    }
}

TEST_CASE("Recovery navigation follows continuous slopes using short supported surface legs", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    for (float slope : {-1.4f, 0.5f, 1.4f})
    {
        ActionPosition from{0,0,0,0}, home{0,20,0,20*slope};
        Search s;
        auto height = [=](ActionPosition const& p) -> std::optional<float> { return slope*p.X; };
        while (s.State == Status::Pending) Advance(s, from, home, height, clear, contains);
        REQUIRE(s.State == Status::Complete);
        auto anchor = from;
        auto legs = Legs(s.Route);
        REQUIRE_FALSE(legs.empty());
        bool followsLargeRise = false;
        for (auto const& leg : legs)
        {
            CHECK(LivingReturnPolicy::Distance(leg, anchor) <= 6);
            followsLargeRise |= std::abs(leg.Z-anchor.Z) > 3.5f;
            Search execution;
            REQUIRE(Advance(execution, anchor, leg, height, clear, contains, 12) == Status::Complete);
            anchor = leg;
        }
        CHECK(anchor.Z == Approx(home.Z));
        if (std::abs(slope) > 1) CHECK(followsLargeRise);
    }
    Search excessive;
    CHECK(Advance(excessive, {0,0,0,0}, {0,481,0,0}, flat, clear, contains) == Status::Rejected);
    Search longSlope;
    auto slope = [](ActionPosition const& p) -> std::optional<float> { return p.X; };
    while (longSlope.State == Status::Pending) Advance(longSlope, {0,0,0,0}, {0,400,0,400}, slope, clear, contains);
    CHECK(longSlope.State == Status::Rejected);
    CHECK(std::string(longSlope.Failure) == "SURFACE_RANGE");
}

TEST_CASE("Recovery navigation resolves raw navmesh clearance against the physical actor floor", "[AIWorld][RecoveryNavigation][RecoveryProjection]")
{
    using namespace RecoveryProjectionPolicy;
    Point actor{0,0,10}, rawMesh{4,0,14.2f};
    Point seed{};
    auto floor = [&](Point const& p) -> std::optional<float> { seed = p; return 12.0f; };
    auto resolved = Resolve(actor, rawMesh, floor);
    REQUIRE(resolved.has_value()); // old raw-height comparison rejected >3 yd
    CHECK(seed.Z == actor.Z); // never query the higher floor using raw mesh Z
    CHECK(resolved->Z == 12);
    CHECK(resolved->X == 4);
    char const* reason = "NONE";
    CHECK_FALSE(Resolve(actor, {6.01f,0,10}, floor, &reason).has_value());
    CHECK(std::string(reason) == "PROJECTION_RANGE");
    CHECK_FALSE(Resolve(actor, {4,0,14.2f}, [](auto const&) -> std::optional<float> { return 14.2f; }, &reason).has_value());
    CHECK(std::string(reason) == "PROJECTION_HEIGHT");
    CHECK_FALSE(Resolve(actor, {4,0,18}, floor, &reason).has_value());
    CHECK(std::string(reason) == "PROJECTION_GROUND_HEIGHT");
}

TEST_CASE("Recovery navigation invalidates changed search geometry and requires surface authorization", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingReturnPolicy;
    HomeCorridorSearch s;
    ActionPosition from{0,40,0,0}, home{0,0,0,0}, danger{0,60,0,0};
    s.Begin(from, home, 14, 96, &danger, 8);
    s.NextTarget = 4; s.NextSurfaceTarget = 2;
    CHECK(s.Matches(from, home, 14, 96, &danger, 8));
    CHECK_FALSE(s.Matches({0,40.1f,0,0}, home, 14, 96, &danger, 8));
    CHECK_FALSE(s.Matches(from, home, 13, 96, &danger, 8));
    CHECK_FALSE(s.Matches(from, home, 14, 97, &danger, 8));
    CHECK_FALSE(s.Matches(from, home, 14, 96, nullptr, 8));
    s.Begin({0,41,0,0}, home, 14, 96, nullptr, 8);
    CHECK(s.NextTarget == 0);
    CHECK(s.NextSurfaceTarget == 0);
    CHECK_FALSE(s.Surface.Started);
    RecoveryMovement nav{{0,4,0,0}, home, 96};
    auto surface = nav;
    surface.SurfaceCorridor = true;
    CHECK_FALSE(nav == surface);
}

TEST_CASE("Recovery navigation examines another polygon when the nearest raw vertex is unusable", "[AIWorld][RecoveryNavigation][RecoveryProjection]")
{
    using namespace RecoveryProjectionPolicy;
    Point actor{0,0,0}, probe{4,0,0};
    // In raw navmesh coordinates the first point is closer to the probe.
    // It is outside the physical circle; the other lies on supported slope.
    Point polygons[]{{6.01f,0,0}, {4,0,4.2f}};
    auto height = [](Point const&) -> std::optional<float> { return 2.0f; };
    auto tile = [](Point const&) { return true; };
    char const* reason = "NONE";
    REQUIRE(std::hypot(polygons[0].X-probe.X,polygons[0].Z) <
        std::hypot(polygons[1].X-probe.X,polygons[1].Z));
    auto result = Nearest(actor, probe, polygons, 2, height, tile, &reason);
    REQUIRE(result.has_value()); // old findNearestPoly returned RANGE immediately
    CHECK(result->Ground.X == 4);
    CHECK(result->Ground.Z == 2);
    CHECK(result->Polygon.Z == 4.2f); // keep raw/physical diagnostic distinction
    CHECK(std::string(reason) == "NONE");
    CHECK_FALSE(Nearest(actor, probe, polygons, 2, height, [](auto const&) { return false; }, &reason).has_value());
    CHECK(std::string(reason) == "MISSING_PROJECTION_TILE");
    CHECK_FALSE(Nearest(actor, probe, polygons, 0, height, tile, &reason).has_value());
    CHECK(std::string(reason) == "NO_GROUND_POLYGON");
}

TEST_CASE("Recovery navigation surface clearance checks lateral walls and vertical body obstacles", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    ActionPosition from{0,0,0,0}, to{0,0.5f,0,0};
    unsigned rays = 0;
    REQUIRE(BodyClear(from, to, 0.5f, 2, [&](auto const&, auto const&) { ++rays; return true; }));
    CHECK(rays == 15);
    // Centre rays all miss this wall, but the actor's right side intersects it.
    auto sideWall = [](auto const& a, auto const& b)
    { return !(a.Y > 0.4f && b.Y > 0.4f && a.X != b.X); };
    CHECK(sideWall(ActionPosition{0,0,0,0.3f}, ActionPosition{0,0.5f,0,0.3f}));
    CHECK_FALSE(BodyClear(from, to, 0.5f, 2, sideWall));
    // A low beam between the chosen ray heights still blocks vertical body
    // clearance at the sample endpoint.
    auto lowBeam = [](auto const& a, auto const& b)
    { return !(a.Z < 1.3f && b.Z > 1.3f && a.X == b.X); };
    CHECK_FALSE(BodyClear(from, to, 0.5f, 2, lowBeam));
    CHECK_FALSE(BodyClear(from, to, -1, 2, clear));
    CHECK_FALSE(BodyClear(from, to, 0.5f, std::numeric_limits<float>::quiet_NaN(), clear));
}

TEST_CASE("Recovery navigation cannot resume a surface route with changed endpoints", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    Search startChanged;
    REQUIRE(Advance(startChanged, {0,0,0,0}, {0,20,0,0}, flat, clear, contains) == Status::Pending);
    CHECK(Advance(startChanged, {0,0.1f,0,0}, {0,20,0,0}, flat, clear, contains) == Status::Rejected);
    CHECK(std::string(startChanged.Failure) == "SURFACE_CONTEXT");
    Search homeChanged;
    REQUIRE(Advance(homeChanged, {0,0,0,0}, {0,20,0,0}, flat, clear, contains) == Status::Pending);
    CHECK(Advance(homeChanged, {0,0,0,0}, {0,21,0,0}, flat, clear, contains) == Status::Rejected);
    CHECK(homeChanged.Route.empty());
}

TEST_CASE("Recovery navigation clears surface authorization when its route is consumed or rejected", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    LivingReturnPolicy::RouteMemory route;
    route.Planned = {{0,5,0,0}, {0,10,0,0}};
    route.SurfaceCorridor = true;
    REQUIRE(route.Advance({0,5,0,0}));
    CHECK(route.SurfaceCorridor);
    REQUIRE(route.Advance({0,10,0,0}));
    CHECK(route.Planned.empty());
    CHECK_FALSE(route.SurfaceCorridor);
    route.Planned = {{0,5,0,0}};
    route.SurfaceCorridor = true;
    route.Reject({0,0,0,0}, {0,5,0,0});
    CHECK(route.Planned.empty());
    CHECK_FALSE(route.SurfaceCorridor);
}

TEST_CASE("Recovery navigation refuses a missing intermediate tile despite installed endpoint tiles", "[AIWorld][RecoveryNavigation][SurfaceCorridor]")
{
    using namespace LivingSurfaceCorridor;
    ActionPosition from{0,0,0,0}, home{0,10,0,0};
    auto hasTile = [](ActionPosition const& p) { return p.X < 5.0f || p.X >= 6.0f; };
    REQUIRE(hasTile(from));
    REQUIRE(hasTile(home));
    Search route;
    REQUIRE(Advance(route, from, home, flat, clear, contains, 8, hasTile) == Status::Pending);
    CHECK(route.Route.back().X == 4);
    REQUIRE(Advance(route, from, home, flat, clear, contains, 8, hasTile) == Status::Rejected);
    CHECK(std::string(route.Failure) == "SURFACE_MISSING_TILE");
    CHECK(route.Route.empty());
    // The executed short leg uses the same check: both of its endpoints can
    // still be in installed tiles while its middle crosses the missing tile.
    Search execution;
    REQUIRE(Advance(execution, {0,4,0,0}, {0,7,0,0}, flat, clear, contains, 12, hasTile) == Status::Rejected);
    CHECK(std::string(execution.Failure) == "SURFACE_MISSING_TILE");
}
