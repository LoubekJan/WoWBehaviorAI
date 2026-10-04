/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingSurfaceDetour.h"
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
