/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingSurfaceDetour.h"
#include "Agent/LivingResidentsV11.h"
#include "Agent/LivingSupplyState.h"
#include <string>

namespace
{
    auto flatDetour = [](ActionPosition const&) -> std::optional<float> { return 0.0f; };
    auto clearDetour = [](ActionPosition const&, ActionPosition const&) { return true; };
    auto boxDetour = [](ActionPosition const& p)
    { return p.X >= -10 && p.X <= 25 && p.Y >= -10 && p.Y <= 10; };
    auto homeBand = [](ActionPosition const& home, float radius)
    {
        return [=](ActionPosition const& p)
        { return std::hypot(p.X-home.X,p.Y-home.Y) <= radius && std::abs(p.Z-home.Z) <= 1; };
    };

    bool CrossesRectangle(ActionPosition const& a, ActionPosition const& b,
        float minX, float maxX, float minY, float maxY)
    {
        float enter = 0, leave = 1;
        auto clip = [&](float source, float delta, float low, float high)
        {
            if (std::abs(delta) < 0.00001f) return source >= low && source <= high;
            float first = (low-source)/delta, last = (high-source)/delta;
            if (first > last) std::swap(first,last);
            enter = std::max(enter,first); leave = std::min(leave,last);
            return enter <= leave;
        };
        return clip(a.X,b.X-a.X,minX,maxX) && clip(a.Y,b.Y-a.Y,minY,maxY);
    }

    auto enclosingWall = [](ActionPosition const& a, ActionPosition const& b)
    {
        return !CrossesRectangle(a,b,0.8f,1.2f,-6.2f,6.2f) &&
            !CrossesRectangle(a,b,-6.2f,1.2f,5.8f,6.2f) &&
            !CrossesRectangle(a,b,-6.2f,1.2f,-6.2f,-5.8f);
    };

    template<class HeightAt, class ClearSegment, class Contains, class IsGoal,
        class HasTile = LivingSurfaceCorridor::InstalledTiles>
    void FinishDetour(LivingSurfaceDetour::Search& search, ActionPosition const& from,
        ActionPosition const& home, float radius, HeightAt&& height, ClearSegment&& clear,
        Contains&& contains, IsGoal&& goal, HasTile&& tile = {})
    {
        unsigned calls = 0;
        while (search.State == LivingSurfaceDetour::Status::Pending && calls < 5000)
        {
            unsigned heights = 0;
            auto countedHeight = [&](ActionPosition const& p) { ++heights; return height(p); };
            LivingSurfaceDetour::Advance(search,from,home,radius,96,countedHeight,clear,contains,goal,8,1,tile);
            CHECK(heights <= 8);
            if (search.State != LivingSurfaceDetour::Status::Complete) CHECK(search.Route.empty());
            CHECK(search.Nodes.size() <= search.NodeLimit);
            CHECK(search.TotalNodes() <= search.NodeLimit);
            CHECK(search.Open.size() <= search.Nodes.size());
            CHECK(search.Index.size() <= search.NodeLimit);
            CHECK(search.EdgeAttempts <= search.EdgeLimit);
            ++calls;
        }
        REQUIRE(search.State != LivingSurfaceDetour::Status::Pending);
    }
}

TEST_CASE("Recovery navigation surface detour plans around a wall including an initial move away from home", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,15,0,0};
    Search search;
    FinishDetour(search,from,home,3,flatDetour,enclosingWall,boxDetour,homeBand(home,3));
    REQUIRE(search.State == Status::Complete);
    REQUIRE_FALSE(search.Route.empty());
    CHECK(std::hypot(search.Route.front().X-home.X,search.Route.front().Y-home.Y) > 15);
    CHECK(homeBand(home,3)(search.Route.back()));
    CHECK(std::string(search.Failure) == "NONE");

    float distance = 0;
    auto previous = from;
    for (auto const& endpoint : search.Route)
    {
        float edge = std::hypot(endpoint.X-previous.X,endpoint.Y-previous.Y,endpoint.Z-previous.Z);
        CHECK(edge > 1);
        CHECK(edge <= 6);
        CHECK(enclosingWall(previous,endpoint));
        LivingSurfaceCorridor::Search execution;
        REQUIRE(LivingSurfaceCorridor::Advance(execution,previous,endpoint,
            flatDetour,enclosingWall,boxDetour,12) == Status::Complete);
        distance += edge; previous = endpoint;
    }
    CHECK(distance <= MaxRouteLength);
    // The source is deliberately absent: consuming the route must walk each
    // checked turn, not skip neighbouring 2.5-yard points as already arrived.
    CHECK_FALSE(Same(search.Route.front(),from));
}

TEST_CASE("Recovery navigation surface detour never publishes a route through a sealed wall", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,15,0,0};
    auto wall = [](ActionPosition const& a, ActionPosition const& b)
    { return !CrossesRectangle(a,b,0.8f,1.2f,-100,100); };
    Search search;
    FinishDetour(search,from,home,3,flatDetour,wall,boxDetour,homeBand(home,3));
    REQUIRE(search.State == Status::Rejected);
    CHECK(std::string(search.Failure) == "SURFACE_DETOUR_EXHAUSTED");
    CHECK(search.Route.empty());
}

TEST_CASE("Recovery navigation surface detour refines an isolated origin on a narrow turning ramp", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0, 0, 0, 0}, home{0, 15, 2.5f, 3};
    auto supported = [](ActionPosition const& p)
    {
        return (p.X >= -0.1f && p.X <= 1.35f && std::abs(p.Y) <= 0.1f) ||
            (std::abs(p.X-1.25f) <= 0.1f && p.Y >= -0.1f && p.Y <= 2.6f) ||
            (p.X >= 1.15f && p.X <= 16 && std::abs(p.Y-2.5f) <= 0.1f);
    };
    auto ramp = [&](ActionPosition const& p) -> std::optional<float>
    { if (!supported(p)) return std::nullopt; return p.X*0.2f; };
    auto goal = [&](ActionPosition const& p)
    { return std::hypot(p.X-home.X,p.Y-home.Y) <= 2 && std::abs(p.Z-p.X*0.2f) < 0.01f; };
    Search search;
    FinishDetour(search, from, home, 2, ramp, clearDetour, supported, goal);
    REQUIRE(search.State == Status::Complete);
    REQUIRE(search.Spacing == NarrowGridStep);
    CHECK(search.EdgeAttempts > 8);
    REQUIRE_FALSE(search.Route.empty());
    CHECK(search.Route.front().X == Approx(1.25f));
    CHECK(search.Route.front().Y == Approx(0));
    auto previous = from;
    for (auto const& endpoint : search.Route)
    {
        CHECK(std::hypot(endpoint.X-previous.X,endpoint.Y-previous.Y,endpoint.Z-previous.Z) > 1);
        LivingSurfaceCorridor::Search execution;
        REQUIRE(LivingSurfaceCorridor::Advance(execution, previous, endpoint,
            ramp, clearDetour, supported, 12) == Status::Complete);
        previous = endpoint;
    }
    CHECK(goal(previous));
}

TEST_CASE("Recovery navigation narrow graph refinement retains edge limits and rejects unsafe escape", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0, 0, 0, 0}, home{0, 15, 0, 0};
    Search search;
    auto blocked = [](ActionPosition const&, ActionPosition const&) { return false; };
    SECTION("sealed body obstacle")
    {
        FinishDetour(search, from, home, 3, flatDetour, blocked, boxDetour, homeBand(home,3));
        CHECK(search.Spacing == NarrowGridStep);
        CHECK(search.EdgeAttempts == 16);
        CHECK(search.Nodes.size() == 1);
        CHECK(std::string(search.Failure) == "SURFACE_DETOUR_EXHAUSTED");
    }
    SECTION("edge limit shared by both spacings")
    {
        search.EdgeLimit = 9;
        FinishDetour(search, from, home, 3, flatDetour, blocked, boxDetour, homeBand(home,3));
        CHECK(search.Spacing == NarrowGridStep);
        CHECK(search.EdgeAttempts == 9);
        CHECK(std::string(search.Failure) == "SURFACE_DETOUR_EDGE_LIMIT");
    }
    SECTION("cliff around the origin")
    {
        auto cliff = [](ActionPosition const& p) -> std::optional<float>
        { return std::hypot(p.X,p.Y) < 0.6f ? 0.0f : -3.0f; };
        FinishDetour(search, from, home, 3, cliff, clearDetour, boxDetour, homeBand(home,3));
        CHECK(search.Spacing == NarrowGridStep);
        CHECK(search.EdgeAttempts == 16);
    }
    REQUIRE(search.State == Status::Rejected);
    CHECK(search.Route.empty());
}

TEST_CASE("Recovery navigation surface detour yields without callbacks and resumes its pending edge", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,20,0,0};
    unsigned callbacks = 0;
    auto height = [&](ActionPosition const&) -> std::optional<float> { ++callbacks; return 0.0f; };
    auto clear = [&](ActionPosition const&,ActionPosition const&) { ++callbacks; return true; };
    auto contains = [&](ActionPosition const& p) { ++callbacks; return boxDetour(p); };
    auto goal = [&](ActionPosition const& p) { ++callbacks; return homeBand(home,3)(p); };
    auto tile = [&](ActionPosition const&) { ++callbacks; return true; };
    Search search;
    CHECK(Advance(search,from,home,3,96,height,clear,contains,goal,0,1,tile) == Status::Pending);
    CHECK(Advance(search,from,home,3,96,height,clear,contains,goal,8,0,tile) == Status::Pending);
    CHECK(callbacks == 0);
    CHECK_FALSE(search.Started);
    REQUIRE(Advance(search,from,home,3,96,height,clear,contains,goal,1,1,tile) == Status::Pending);
    CHECK(search.Started);
    REQUIRE(Advance(search,from,home,3,96,height,clear,contains,goal,1,1,tile) == Status::Pending);
    REQUIRE(search.EdgeActive);
    REQUIRE(search.Edge.Started);
    CHECK(search.Edge.NextSample == 1);
    auto before = callbacks;
    CHECK(Advance(search,from,home,3,96,height,clear,contains,goal,0,1,tile) == Status::Pending);
    CHECK(callbacks == before);
    CHECK(search.EdgeAttempts == 1);
    REQUIRE(Advance(search,from,home,3,96,height,clear,contains,goal,1,1,tile) == Status::Pending);
    CHECK(search.Edge.Started);
    CHECK(search.Edge.NextSample == 2);
    FinishDetour(search,from,home,3,height,clear,contains,goal,tile);
    REQUIRE(search.State == Status::Complete);
    CHECK(homeBand(home,3)(search.Route.back()));
}

TEST_CASE("Recovery navigation surface detour rejects changed geometry rather than reusing a prefix", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,20,0,0};
    Search search;
    REQUIRE(Advance(search,from,home,3,96,flatDetour,clearDetour,boxDetour,homeBand(home,3)) == Status::Pending);
    SECTION("actual source") { from.X += 0.1f; }
    SECTION("home") { home.Z += 0.1f; }
    float radius = 3, limit = 96;
    SECTION("arrival band") { radius = 2.9f; }
    SECTION("episode bounds") { limit = 95; }
    SECTION("search limits") { --search.EdgeLimit; }
    CHECK(Advance(search,from,home,radius,limit,flatDetour,clearDetour,boxDetour,homeBand(home,radius)) == Status::Rejected);
    CHECK(std::string(search.Failure) == "SURFACE_DETOUR_CONTEXT");
    CHECK(search.Route.empty());
}

TEST_CASE("Recovery navigation surface detour cannot change floors or cross cliffs tiles holes boundaries or danger", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,15,0,0};
    Search search;
    auto contains = [](ActionPosition const& p) { return p.X >= -5 && p.X <= 20 && std::abs(p.Y) <= 1; };
    SECTION("unsupported actual start")
    {
        from.Z = 9;
        FinishDetour(search,from,home,3,flatDetour,clearDetour,contains,homeBand(home,3));
        CHECK(std::string(search.Failure) == "SURFACE_START_HEIGHT");
    }
    SECTION("home on another floor")
    {
        home.Z = 9;
        FinishDetour(search,from,home,3,flatDetour,clearDetour,contains,homeBand(home,3));
    }
    SECTION("cliff")
    {
        auto floor = [](ActionPosition const& p) -> std::optional<float> { return p.X < 1 ? 0.0f : 4.0f; };
        FinishDetour(search,from,home,3,floor,clearDetour,contains,homeBand(home,3));
    }
    SECTION("missing intermediate tile")
    {
        auto tile = [](ActionPosition const& p) { return p.X < 1 || p.X > 2; };
        FinishDetour(search,from,home,3,flatDetour,clearDetour,contains,homeBand(home,3),tile);
    }
    SECTION("floor hole")
    {
        auto floor = [](ActionPosition const& p) -> std::optional<float>
        { if (p.X >= 1 && p.X <= 2) return std::nullopt; return 0.0f; };
        FinishDetour(search,from,home,3,floor,clearDetour,contains,homeBand(home,3));
    }
    SECTION("excluded intermediate bounds")
    {
        auto bounds = [&](ActionPosition const& p) { return contains(p) && (p.X < 1 || p.X > 2); };
        FinishDetour(search,from,home,3,flatDetour,clearDetour,bounds,homeBand(home,3));
    }
    SECTION("danger intersection")
    {
        auto danger = [](ActionPosition const& a, ActionPosition const& b)
        { return !CrossesRectangle(a,b,0.9f,2.1f,-5,5); };
        FinishDetour(search,from,home,3,flatDetour,danger,contains,homeBand(home,3));
    }
    REQUIRE(search.State == Status::Rejected);
    CHECK(search.Route.empty());
}

TEST_CASE("Recovery navigation surface detour limits nodes edges route length and incompatible maps", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,20,0,0};
    Search search;
    SECTION("nodes")
    {
        search.NodeLimit = 2;
        FinishDetour(search,from,home,3,flatDetour,clearDetour,boxDetour,homeBand(home,3));
        CHECK(std::string(search.Failure) == "SURFACE_DETOUR_NODE_LIMIT");
        CHECK(search.Nodes.size() == 2);
    }
    SECTION("edges")
    {
        search.EdgeLimit = 1;
        FinishDetour(search,from,home,3,flatDetour,clearDetour,boxDetour,homeBand(home,3));
        CHECK(std::string(search.Failure) == "SURFACE_DETOUR_EDGE_LIMIT");
        CHECK(search.EdgeAttempts == 1);
    }
    SECTION("route")
    {
        home.X = 500;
        CHECK(Advance(search,from,home,3,600,flatDetour,clearDetour,[](auto const&) { return true; },homeBand(home,3)) == Status::Rejected);
        CHECK(std::string(search.Failure) == "SURFACE_DETOUR_RANGE");
    }
    SECTION("map")
    {
        home.MapId = 1;
        CHECK(Advance(search,from,home,3,96,flatDetour,clearDetour,boxDetour,homeBand(home,3)) == Status::Rejected);
    }
    REQUIRE(search.State == Status::Rejected);
    CHECK(search.Route.empty());
}

TEST_CASE("Recovery navigation surface detour preserves physically executable corners on supported slopes", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,20,20,0};
    auto floorZ = [](ActionPosition const& p) { return 1.4f*(p.X+p.Y)/std::sqrt(2.0f); };
    auto slope = [&](ActionPosition const& p) -> std::optional<float>
    {
        float height = floorZ(p);
        // Model the real provider's height-query seed window. A single query
        // at a whole edge's endpoint misses even this walkable uphill floor.
        if (height > p.Z+0.8f || height < p.Z-3) return std::nullopt;
        return height;
    };
    home.Z = floorZ(home);
    CHECK_FALSE(slope({0,2.5f,0,0}).has_value());
    auto bounds = [](ActionPosition const& p) { return p.X >= -5 && p.Y >= -5 && p.X <= 25 && p.Y <= 25; };
    auto goal = [&](ActionPosition const& p)
    { return std::hypot(p.X-home.X,p.Y-home.Y) <= 3 && std::abs(p.Z-floorZ(p)) < 0.01f; };
    Search search;
    FinishDetour(search,from,home,3,slope,clearDetour,bounds,goal);
    REQUIRE(search.State == Status::Complete);
    REQUIRE_FALSE(search.Route.empty());
    auto previous = from;
    for (auto const& endpoint : search.Route)
    {
        CHECK(std::hypot(endpoint.X-previous.X,endpoint.Y-previous.Y,endpoint.Z-previous.Z) <= 6);
        LivingSurfaceCorridor::Search execution;
        REQUIRE(LivingSurfaceCorridor::Advance(execution,previous,endpoint,slope,clearDetour,bounds,12) == Status::Complete);
        previous = endpoint;
    }
    CHECK(goal(search.Route.back()));
}
namespace
{
    auto roomBounds = [](ActionPosition const& p)
    { return p.X >= -3 && p.X <= 3 && p.Y >= -3 && p.Y <= 14; };
    auto roomDoor = [](ActionPosition const& a, ActionPosition const& b)
    {
        return LivingSurfaceCorridor::BodyClear(a,b,.3f,2.4f,
            [](ActionPosition const& from, ActionPosition const& to)
            {
                return !CrossesRectangle(from,to,-3.3f,.65f,3.0f,3.3f) &&
                    !CrossesRectangle(from,to,1.85f,3.3f,3.0f,3.3f);
            });
    };
}

TEST_CASE("Recovery refines an exhausted multi-cell room to find its supported narrow doorway", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,0,12,0};
    Search search;
    FinishDetour(search,from,home,1.9f,flatDetour,roomDoor,roomBounds,homeBand(home,1.9f));
    REQUIRE(search.State == Status::Complete);
    REQUIRE(search.Spacing == NarrowGridStep);
    REQUIRE(search.RetiredNodes > 1); // Coarse exploration found a room, not an isolated origin.
    REQUIRE(search.TotalNodes() <= search.NodeLimit);
    REQUIRE(search.EdgeAttempts <= search.EdgeLimit);
    REQUIRE_FALSE(search.Route.empty());
    auto previous = from;
    for (auto const& point : search.Route)
    {
        REQUIRE(roomDoor(previous,point));
        LivingSurfaceCorridor::Search execution;
        REQUIRE(LivingSurfaceCorridor::Advance(execution,previous,point,
            flatDetour,roomDoor,roomBounds,12) == Status::Complete);
        previous = point;
    }
    REQUIRE(homeBand(home,1.9f)(previous));
}

TEST_CASE("Room refinement retains shared work caps and never restarts a changed or yielded query", "[AIWorld][RecoveryNavigation][SurfaceDetour]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{0,0,0,0}, home{0,0,12,0};
    auto goal = homeBand(home,1.9f);
    Search baseline;
    unsigned calls = 0;
    while (baseline.Spacing == GridStep && baseline.State == Status::Pending && ++calls < 1000)
        Advance(baseline,from,home,1.9f,96,flatDetour,roomDoor,roomBounds,goal,8,1);
    REQUIRE(baseline.Spacing == NarrowGridStep);
    REQUIRE(baseline.RetiredNodes > 1);
    REQUIRE(baseline.EdgeAttempts > 1);
    Search search;
    SECTION("coarse edges cannot be spent again on the fine graph")
    {
        search.EdgeLimit = baseline.EdgeAttempts - 1;
        FinishDetour(search,from,home,1.9f,flatDetour,roomDoor,roomBounds,goal);
        REQUIRE(search.State == Status::Rejected);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_EDGE_LIMIT");
        REQUIRE(search.EdgeAttempts == search.EdgeLimit);
        REQUIRE(search.Route.empty());
    }
    SECTION("retired coarse cells remain inside the same node cap")
    {
        search.NodeLimit = baseline.RetiredNodes + 2;
        FinishDetour(search,from,home,1.9f,flatDetour,roomDoor,roomBounds,goal);
        REQUIRE(search.State == Status::Rejected);
        REQUIRE(search.RetiredNodes > 1);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_NODE_LIMIT");
        REQUIRE(search.TotalNodes() == search.NodeLimit);
        REQUIRE(search.Route.empty());
    }
    SECTION("zero geometry budgets preserve the refined cursor exactly")
    {
        search = baseline;
        auto nodes = search.TotalNodes(), edges = search.EdgeAttempts;
        unsigned callbacks = 0;
        auto height = [&](ActionPosition const&) -> std::optional<float> { ++callbacks; return 0; };
        REQUIRE(Advance(search,from,home,1.9f,96,height,roomDoor,roomBounds,goal,0,1) == Status::Pending);
        REQUIRE(Advance(search,from,home,1.9f,96,height,roomDoor,roomBounds,goal,8,0) == Status::Pending);
        REQUIRE(callbacks == 0);
        REQUIRE(search.TotalNodes() == nodes);
        REQUIRE(search.EdgeAttempts == edges);
        REQUIRE(search.Spacing == NarrowGridStep);
    }
    SECTION("a changed goal cannot borrow the old room or door proof")
    {
        search = baseline;
        home.X = 1;
        REQUIRE(Advance(search,from,home,1.9f,96,flatDetour,roomDoor,roomBounds,goal,8,1) == Status::Rejected);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_CONTEXT");
        REQUIRE(search.Route.empty());
    }
    SECTION("a sealed doorway stays rejected after its one refinement")
    {
        auto sealed = [](ActionPosition const& a, ActionPosition const& b)
        { return !CrossesRectangle(a,b,-4,4,3,3.3f); };
        FinishDetour(search,from,home,1.9f,flatDetour,sealed,roomBounds,goal);
        REQUIRE(search.State == Status::Rejected);
        REQUIRE(search.RetiredNodes > 1);
        REQUIRE(search.Spacing == NarrowGridStep);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_EXHAUSTED");
        REQUIRE(search.Route.empty());
    }
    SECTION("a doorway cliff stays rejected with unchanged floor checks")
    {
        auto cliff = [](ActionPosition const& p) -> std::optional<float> { return p.Y < 3 ? 0 : -3; };
        FinishDetour(search,from,home,1.9f,cliff,roomDoor,roomBounds,goal);
        REQUIRE(search.State == Status::Rejected);
        REQUIRE(search.RetiredNodes > 1);
        REQUIRE(search.Route.empty());
    }
}

TEST_CASE("A bounded doorway departure stages through the existing warehouse before a distant physical field", "[AIWorld][RecoveryNavigation][SurfaceDetour][LivingResidentsV11]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition home{725,0,0,0}, field{725,100,12,0}, warehouse{725,0,8,0};
    auto bounds = [](ActionPosition const& p)
    { return p.X >= -3 && p.X <= 104 && p.Y >= -3 && p.Y <= 14 && (p.Y >= 3 || p.X <= 3); };
    auto door = [](ActionPosition const& a, ActionPosition const& b)
    {
        return LivingSurfaceCorridor::BodyClear(a,b,.3f,2.4f,
            [](ActionPosition const& from, ActionPosition const& to)
            {
                return !CrossesRectangle(from,to,-4,.65f,3,3.3f) &&
                    !CrossesRectangle(from,to,1.85f,105,3,3.3f);
            });
    };
    auto finish = [&](Search& search, ActionPosition const& from, ActionPosition const& goal)
    {
        unsigned calls = 0;
        while (search.State == Status::Pending && ++calls < 5000)
            Advance(search,from,goal,1.9f,160,flatDetour,door,bounds,homeBand(goal,1.9f),8,1);
        REQUIRE(search.State != Status::Pending);
        REQUIRE(search.TotalNodes() <= search.NodeLimit);
        REQUIRE(search.EdgeAttempts <= search.EdgeLimit);
    };
    // A complete long proof cannot be produced within this request's cap.
    // Nothing, including the otherwise safe doorway prefix, is dispatched.
    Search direct; direct.NodeLimit = 256;
    finish(direct,home,field);
    REQUIRE(direct.State == Status::Rejected);
    REQUIRE(direct.Route.empty());
    LivingResidentsV11::State departure;
    REQUIRE(LivingResidentsV11::ShouldStageDeparture(LivingResidentsV11::Job::Farmer,
        departure,home,home,1,0,0,false));
    LivingResidentsV11::BeginDepartureStage(departure);
    LivingSupply::Ledger food;
    for (uint32 id = 1; id <= 6; ++id) REQUIRE(food.ConfigureDepot(id));
    REQUIRE(food.ConfigureResidentsV11());
    REQUIRE(food.ConfigureField(1,1));
    REQUIRE(food.RegisterFarmer(902000,1,1));
    auto physical = home;
    auto execute = [&](Search const& route)
    {
        INFO("stage from=" << route.From.X << "," << route.From.Y << " to=" << route.Home.X << "," << route.Home.Y
            << " failure=" << route.Failure << " nodes=" << route.TotalNodes() << " retired=" << route.RetiredNodes
            << " edges=" << route.EdgeAttempts << " spacing=" << route.Spacing);
        REQUIRE(route.State == Status::Complete);
        for (auto const& point : route.Route)
        {
            LivingSurfaceCorridor::Search segment;
            REQUIRE(LivingSurfaceCorridor::Advance(segment,physical,point,flatDetour,door,bounds,12) == Status::Complete);
            physical = point; // Each executed edge has its full floor/body proof.
        }
    };
    Search first; first.NodeLimit = 256;
    finish(first,physical,warehouse);
    execute(first);
    REQUIRE(homeBand(warehouse,1.9f)(physical));
    REQUIRE(food.DepotFood() == 0);
    REQUIRE(food.GetCargo(902000) == 0);
    REQUIRE(food.GetCounters().HarvestedTotal == 0);
    departure.DepartureViaDepot = false;
    Search onward; onward.NodeLimit = 256;
    finish(onward,physical,field);
    execute(onward);
    REQUIRE(homeBand(field,1.9f)(physical));
    // Route selection and a warehouse pass-through create no food. Only
    // the subsequent owned completed work receipt extracts the crop.
    REQUIRE(food.DepotFood() == 0);
    REQUIRE(food.HarvestField(902000,1,1,1,1,true));
    Search returning; returning.NodeLimit = 256;
    finish(returning,physical,warehouse);
    execute(returning);
    REQUIRE(food.Deliver(902000,2,1,1,true));
    REQUIRE(food.GetDepot(1)->Stock == 1);
    REQUIRE(food.Conserved());
    REQUIRE(food.PotentialConserved());
}

TEST_CASE("Departure staging never borrows budget yields cargo or a foreign home episode", "[AIWorld][LivingResidentsV11]")
{
    using namespace LivingResidentsV11;
    ActionPosition home{725,0,0,0}, doorway{725,0,4,0};
    State state;
    REQUIRE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,0,0,false));
    REQUIRE(ShouldStageDeparture(Job::Carrier,state,doorway,home,5,1,0,false));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,0,0,true));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,0,1,false));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Consumer,state,doorway,home,5,1,0,false));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Raider,state,doorway,home,6,1,0,false));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,1,0,false));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Carrier,state,{725,20,0,0},home,5,1,0,false));
    REQUIRE_FALSE(ShouldStageDeparture(Job::Carrier,state,{0,0,4,0},home,5,1,0,false));
    state.ReturningHome = true;
    REQUIRE_FALSE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,0,0,false));
    state.ReturningHome = false; state.RestAtHome = true;
    REQUIRE_FALSE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,0,0,false));
    state.RestAtHome = false; state.DepartureTried = true;
    REQUIRE_FALSE(ShouldStageDeparture(Job::Farmer,state,doorway,home,1,0,0,false));
}

TEST_CASE("Strict resident arrival continues past a real XY-near node outside the 3D meal or home radius", "[AIWorld][RecoveryNavigation][SurfaceDetour][LivingResidentsV11]")
{
    using namespace LivingReturnPolicy;
    using namespace LivingSurfaceDetour;
    ActionPosition from{725,110.5215f,1017.915f,.2463253f};
    ActionPosition home{725,101.1432f,1020.631f,.9810646f};
    ActionPosition rejected{725,103.0215f,1020.415f,.719072192f};
    REQUIRE(std::hypot(rejected.X-home.X,rejected.Y-home.Y) < 1.9f);
    REQUIRE(Distance(rejected,home) > 1.9f);
    REQUIRE(HomeEndpointMatches(home,1.9f,rejected,rejected)); // unchanged legacy XY contract
    REQUIRE_FALSE(HomeEndpointMatches(home,1.9f,rejected,rejected,true,true));
    float slopeY = (rejected.Z-from.Z - .06f*(from.X-rejected.X))/(rejected.Y-from.Y);
    auto floor = [&](ActionPosition const& p) -> std::optional<float>
    { return from.Z + .06f*(from.X-p.X) + slopeY*(p.Y-from.Y); };
    auto bounds = [&](ActionPosition const& p)
    { return p.X >= 95 && p.X <= 115 && p.Y >= 1010 && p.Y <= 1030; };
    unsigned rejectedNearNodes = 0;
    auto goal = [&](ActionPosition const& p)
    {
        bool match = HomeEndpointMatches(home,1.9f,p,p,true,true);
        if (!match && std::hypot(p.X-home.X,p.Y-home.Y) <= 1.9f) ++rejectedNearNodes;
        return match;
    };
    Search route;
    FinishDetour(route,from,home,1.9f,floor,clearDetour,bounds,goal);
    REQUIRE(route.State == Status::Complete);
    REQUIRE(rejectedNearNodes > 0);
    REQUIRE_FALSE(route.Route.empty());
    REQUIRE(Distance(route.Route.back(),home) <= 1.9f);
    REQUIRE_FALSE(SamePosition(route.Route.back(),rejected));
    auto previous = from;
    for (auto const& point : route.Route)
    {
        LivingSurfaceCorridor::Search execution;
        REQUIRE(LivingSurfaceCorridor::Advance(execution,previous,point,floor,clearDetour,bounds,12) == Status::Complete);
        previous = point;
    }
}

TEST_CASE("Strict resident endpoint checks require sight and isolate cached legacy arrival proofs", "[AIWorld][RecoveryNavigation][LivingResidentsV11]")
{
    using namespace LivingReturnPolicy;
    ActionPosition from{725,10,0,0}, home{725,0,0,0}, near{725,1,0,0};
    REQUIRE(HomeEndpointMatches(home,1.9f,near,near));
    REQUIRE_FALSE(HomeEndpointMatches(home,1.9f,near,near,true,false));
    REQUIRE_FALSE(HomeEndpointMatches(home,1.9f,near,near,true)); // missing sight proof fails closed
    REQUIRE(HomeEndpointMatches(home,1.9f,near,near,true,true));
    HomeCorridorSearch cache;
    cache.Begin(from,home,1.9f,384,nullptr,8,true);
    REQUIRE(cache.Matches(from,home,1.9f,384,nullptr,8,true));
    REQUIRE_FALSE(cache.Matches(from,home,1.9f,384,nullptr,8,true,true));
    cache.Begin(from,home,1.9f,384,nullptr,8,true,true);
    REQUIRE(cache.StrictArrival);
    REQUIRE(cache.Matches(from,home,1.9f,384,nullptr,8,true,true));
    REQUIRE_FALSE(cache.Matches(from,home,1.9f,384,nullptr,8,true));
}

namespace
{
    auto fineDoorBounds = [](ActionPosition const& p)
    { return p.X >= -3 && p.X <= 5 && p.Y >= -3 && p.Y <= 10; };
    auto fineDoorFloor = [](ActionPosition const& p) -> std::optional<float>
    { return p.Y < 2.5f ? 0 : p.Y < 3.0f ? .25f : p.Y < 3.5f ? .5f : .75f; };
    auto fineDoorBody = [](ActionPosition const& a, ActionPosition const& b)
    {
        return LivingSurfaceCorridor::BodyClear(a,b,.2f,2.031f,
            [](ActionPosition const& from, ActionPosition const& to)
            {
                // The .625-yard centre is deliberately between both older
                // grids. A radius-.2 body fits this .825-yard stair doorway.
                return !CrossesRectangle(from,to,-4,.15f,3.0f,3.3f) &&
                    !CrossesRectangle(from,to,.975f,6,3.0f,3.3f);
            });
    };
}

TEST_CASE("Opted-in mixed graph keeps immutable physical proof through a sub-yard stair doorway", "[AIWorld][RecoveryNavigation][SurfaceDetour][FineTurns]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{725,0,0,0}, home{725,0,8,.75f};
    auto goal = [&](ActionPosition const& p)
    { return LivingReturnPolicy::HomeEndpointMatches(home,1.9f,p,p,true,true); };
    Search legacy;
    FinishDetour(legacy,from,home,1.9f,fineDoorFloor,fineDoorBody,fineDoorBounds,goal);
    REQUIRE(legacy.State == Status::Rejected);
    REQUIRE(legacy.Spacing == NarrowGridStep);
    REQUIRE(legacy.Route.empty());
    Search search; search.FineTurnsEnabled = true;
    unsigned calls = 0;
    while (search.State == Status::Pending && ++calls < 5000)
    {
        auto before = search;
        unsigned samples = 0;
        auto height = [&](ActionPosition const& p) { ++samples; return fineDoorFloor(p); };
        Advance(search,from,home,1.9f,96,height,fineDoorBody,fineDoorBounds,goal,8,1);
        REQUIRE(samples <= 8);
        REQUIRE(search.TotalNodes() <= search.NodeLimit);
        REQUIRE(search.EdgeAttempts <= search.EdgeLimit);
        REQUIRE(search.EdgeAttempts >= before.EdgeAttempts);
        REQUIRE(search.TotalNodes() >= before.TotalNodes());
        for (std::size_t i = 0; i < before.Nodes.size(); ++i)
        {
            auto const& old = before.Nodes[i]; auto const& node = search.Nodes[i];
            REQUIRE(node.X == old.X);
            REQUIRE(node.Y == old.Y);
            if (old.Closed)
            {
                REQUIRE(node.Closed);
                REQUIRE(Same(old.Position,node.Position));
                REQUIRE(old.Cost == node.Cost);
                REQUIRE(old.Parent == node.Parent);
            }
        }
        REQUIRE(search.Spacing == FineTurnGridStep);
        REQUIRE(search.RetiredNodes == 0);
        if (search.State != Status::Complete) REQUIRE(search.Route.empty());
    }
    REQUIRE(search.State == Status::Complete);
    REQUIRE(search.Spacing == FineTurnGridStep);
    REQUIRE(search.RetiredNodes == 0);
    REQUIRE(goal(search.Route.back()));
    bool shortTurn = false, coarseTravel = false, changedDirection = false;
    ActionPosition previous = from, beforePrevious = from;
    for (auto const& point : search.Route)
    {
        float length = LivingReturnPolicy::Distance(previous,point);
        shortTurn = shortTurn || length < 1.0f;
        coarseTravel = coarseTravel || length >= GridStep;
        float cross = (previous.X-beforePrevious.X)*(point.Y-previous.Y) -
            (previous.Y-beforePrevious.Y)*(point.X-previous.X);
        changedDirection = changedDirection || std::abs(cross) > .01f;
        LivingSurfaceCorridor::Search edge;
        REQUIRE(LivingSurfaceCorridor::Advance(edge,previous,point,fineDoorFloor,
            fineDoorBody,fineDoorBounds,12) == Status::Complete);
        beforePrevious = previous; previous = point;
    }
    REQUIRE(shortTurn);
    REQUIRE(coarseTravel);
    REQUIRE(changedDirection);
}

TEST_CASE("Mixed edge retries preserve their cursor context and shared work admission", "[AIWorld][RecoveryNavigation][SurfaceDetour][FineTurns]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{725,0,0,0}, home{725,0,8,.75f};
    auto goal = homeBand(home,1.9f);
    Search baseline; baseline.FineTurnsEnabled = true;
    unsigned calls = 0;
    while (baseline.NextStride == 4 && baseline.State == Status::Pending && ++calls < 1000)
        Advance(baseline,from,home,1.9f,96,fineDoorFloor,fineDoorBody,fineDoorBounds,goal,8,1);
    REQUIRE(baseline.State == Status::Pending);
    REQUIRE(baseline.NextStride < 4);
    REQUIRE_FALSE(baseline.EdgeActive);
    Search search;
    SECTION("zero geometry allowance cannot spend or advance a shorter retry")
    {
        search = baseline;
        auto before = search;
        unsigned callbacks = 0;
        auto height = [&](ActionPosition const&) -> std::optional<float> { ++callbacks; return 0; };
        REQUIRE(Advance(search,from,home,1.9f,96,height,fineDoorBody,fineDoorBounds,goal,0,1) == Status::Pending);
        REQUIRE(Advance(search,from,home,1.9f,96,height,fineDoorBody,fineDoorBounds,goal,8,0) == Status::Pending);
        REQUIRE(callbacks == 0);
        REQUIRE(search.Spacing == before.Spacing);
        REQUIRE(search.TotalNodes() == before.TotalNodes());
        REQUIRE(search.EdgeAttempts == before.EdgeAttempts);
        REQUIRE(search.Current == before.Current);
        REQUIRE(search.NextDirection == before.NextDirection);
        REQUIRE(search.NextStride == before.NextStride);
        REQUIRE(search.EdgeStride == before.EdgeStride);
        REQUIRE(search.Open == before.Open);
    }
    SECTION("a proof cannot change to legacy execution after starting")
    {
        search = baseline; search.FineTurnsEnabled = false;
        REQUIRE(Advance(search,from,home,1.9f,96,fineDoorFloor,fineDoorBody,fineDoorBounds,goal) == Status::Rejected);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_CONTEXT");
        REQUIRE(search.Route.empty());
    }
    SECTION("a different physical origin cannot inherit its old stair proof")
    {
        search = baseline; from.X += .25f;
        REQUIRE(Advance(search,from,home,1.9f,96,fineDoorFloor,fineDoorBody,fineDoorBounds,goal) == Status::Rejected);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_CONTEXT");
        REQUIRE(search.Route.empty());
    }
    SECTION("node admission is shared by coarse and fine physical edges")
    {
        search.FineTurnsEnabled = true; search.NodeLimit = 2;
        FinishDetour(search,from,home,1.9f,fineDoorFloor,fineDoorBody,fineDoorBounds,goal);
        REQUIRE(search.State == Status::Rejected);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_NODE_LIMIT");
        REQUIRE(search.TotalNodes() <= search.NodeLimit);
        REQUIRE(search.Route.empty());
    }
    SECTION("edge admission cannot be reset by a shorter physical retry")
    {
        search.FineTurnsEnabled = true; search.EdgeLimit = 2;
        auto blocked = [](ActionPosition const&, ActionPosition const&) { return false; };
        FinishDetour(search,from,home,1.9f,fineDoorFloor,blocked,fineDoorBounds,goal);
        REQUIRE(search.State == Status::Rejected);
        REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_EDGE_LIMIT");
        REQUIRE(search.EdgeAttempts == search.EdgeLimit);
        REQUIRE(search.Nodes.size() == 1);
        REQUIRE(search.Route.empty());
    }
}

TEST_CASE("Mixed graph refines only rejected physical edges and never retries missing tiles or bounds", "[AIWorld][RecoveryNavigation][SurfaceDetour][FineTurns]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{725,0,0,0}, home{725,0,8,0};
    Search search; search.FineTurnsEnabled = true;
    auto bounds = [](ActionPosition const&) { return true; };
    auto goal = homeBand(home,1.9f);
    SECTION("each sealed direction spends exactly three physical attempts")
    {
        auto blocked = [](ActionPosition const&, ActionPosition const&) { return false; };
        FinishDetour(search,from,home,1.9f,flatDetour,blocked,bounds,goal);
        REQUIRE(search.EdgeAttempts == 24);
        REQUIRE(search.Nodes.size() == 1);
    }
    SECTION("a missing tile does not create a finer query")
    {
        auto tile = [&](ActionPosition const& p) { return Same(p,from); };
        FinishDetour(search,from,home,1.9f,flatDetour,clearDetour,bounds,goal,tile);
        REQUIRE(search.EdgeAttempts == 8);
    }
    SECTION("excluded physical bounds do not create a finer query")
    {
        auto onlyOrigin = [&](ActionPosition const& p) { return Same(p,from); };
        FinishDetour(search,from,home,1.9f,flatDetour,clearDetour,onlyOrigin,goal);
        REQUIRE(search.EdgeAttempts == 8);
    }
    REQUIRE(search.State == Status::Rejected);
    REQUIRE(std::string(search.Failure) == "SURFACE_DETOUR_EXHAUSTED");
    REQUIRE(search.Route.empty());
    REQUIRE(search.RetiredNodes == 0);
}

TEST_CASE("Fine offsets reconnect to a single coarse grid on both sides of the source", "[AIWorld][RecoveryNavigation][SurfaceDetour][FineTurns]")
{
    using namespace LivingSurfaceDetour;
    // Shared anchors stop every local stair offset spawning another whole
    // outdoor lattice. Half-cell ties must agree with the physical replay.
    REQUIRE(CoarseAxis(2,0) == 4);
    REQUIRE(CoarseAxis(-2,0) == -4);
    REQUIRE(CoarseAxis(0,1) == 4);
    REQUIRE(CoarseAxis(0,-1) == -4);
    for (int coordinate = -32; coordinate <= 32; ++coordinate)
        for (int direction = -1; direction <= 1; ++direction)
        {
            int anchor = CoarseAxis(coordinate,direction);
            REQUIRE(anchor % 4 == 0);
            REQUIRE(std::abs(anchor-coordinate) <= 4);
            if (direction) REQUIRE((anchor-coordinate)*direction > 0);
            else REQUIRE(std::abs(anchor-coordinate) <= 2);
        }
}

TEST_CASE("Fine-turn routing never converts an unsafe wall floor or tile into a passage", "[AIWorld][RecoveryNavigation][SurfaceDetour][FineTurns]")
{
    using namespace LivingSurfaceDetour;
    ActionPosition from{725,0,0,0}, home{725,0,8,.75f};
    auto goal = [&](ActionPosition const& p)
    { return LivingReturnPolicy::HomeEndpointMatches(home,1.9f,p,p,true,true); };
    Search search; search.FineTurnsEnabled = true;
    SECTION("sealed stair door")
    {
        auto sealed = [](ActionPosition const& a, ActionPosition const& b)
        { return !CrossesRectangle(a,b,-4,6,3.0f,3.3f); };
        FinishDetour(search,from,home,1.9f,fineDoorFloor,sealed,fineDoorBounds,goal);
    }
    SECTION("cliff through the door")
    {
        auto cliff = [](ActionPosition const& p) -> std::optional<float> { return p.Y < 3 ? 0 : -3; };
        FinishDetour(search,from,home,1.9f,cliff,fineDoorBody,fineDoorBounds,goal);
    }
    SECTION("missing intermediate map tile")
    {
        auto tile = [](ActionPosition const& p) { return p.Y < 2.75f || p.Y > 3.5f; };
        FinishDetour(search,from,home,1.9f,fineDoorFloor,fineDoorBody,fineDoorBounds,goal,tile);
    }
    SECTION("another supported floor at the same XY")
    {
        home.Z = 6;
        FinishDetour(search,from,home,1.9f,fineDoorFloor,fineDoorBody,fineDoorBounds,goal);
    }
    REQUIRE(search.State == Status::Rejected);
    REQUIRE(search.Route.empty());
    REQUIRE(search.TotalNodes() <= search.NodeLimit);
    REQUIRE(search.EdgeAttempts <= search.EdgeLimit);
}
