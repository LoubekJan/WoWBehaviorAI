/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "CompleteChasePath.h"
#include "GroundedPathSupport.h"
#include "Spline/MoveSplineFlag.h"
#include "Agent/LivingSurfaceCorridor.h"
#include <limits>
#include <vector>

namespace
{
    struct GroundPoint { float x, y, z; };
    using GroundPoints = std::vector<GroundPoint>;
    auto clearGround = [](GroundPoint const&, GroundPoint const&) { return true; };
    auto inGroundBounds = [](GroundPoint const&) { return true; };
    auto installedGround = [](GroundPoint const&) { return true; };
    auto flatGround = [](GroundPoint const&) -> std::optional<float> { return 0.0f; };
    bool clearGroundBody(GroundPoint const& a, GroundPoint const& b)
    {
        return LivingSurfaceCorridor::BodyClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z},0.5f,2,
            [](ActionPosition const&,ActionPosition const&) { return true; });
    }
    struct GroundHuntFixture
    {
        GroundPoints Points{{0,0,29.58867f},{4,0,29.58867f}};
        GroundPoints Shortened{{0,0,29.58867f},{2,0,29.58867f}};
        GroundPoint CalculatedFrom{};
        bool ShortenedOnce = false;
        void AllowSteepSlopes() { }
        bool CalculatePathFrom(GroundPoint const& origin, GroundPoint const&)
        { CalculatedFrom = origin; return true; }
        PathType GetPathType() const { return PATHFIND_NORMAL; }
        GroundPoints const& GetPath() const { return Points; }
        void ShortenPathUntilDist(GroundPoint const&,float) { ShortenedOnce = true; Points = Shortened; }
    };
}

TEST_CASE("Recovery navigation keeps validated grounded splines linear despite inherited flight capability", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    using Flag = Movement::MoveSplineFlag;
    uint32 const preserved = Flag::Final_Target | Flag::CanSwim | Flag::Backward | Flag::OrientationFixed;
    // MoveSplineInit can inherit Flying from CAN_FLY even while IsFlying()
    // is false. Both possible smoothing bits must be removed for a checked
    // ground route, without changing facing or swimming capability.
    for (uint32 smooth : {uint32(Flag::Flying), uint32(Flag::Catmullrom), uint32(Flag::Mask_CatmullRom)})
    {
        Flag flags(preserved | smooth);
        REQUIRE(flags.isSmooth());
        flags.EnableLinear();
        CHECK(flags.isLinear());
        CHECK_FALSE(flags.isSmooth());
        CHECK(flags.raw() == preserved);
        flags.EnableLinear();
        CHECK(flags.raw() == preserved);
        // Existing native flight remains available when explicitly requested.
        flags.EnableFlying();
        CHECK(flags.isSmooth());
        CHECK(flags.hasAllFlags(preserved));
    }
}

TEST_CASE("Recovery navigation validates the actual spline source during deferred map cell relocation", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    // UpdateSplinePosition can queue a new cell while the actor's cached XYZ
    // still reports its old cell. Launch uses ComputePosition, not that cache.
    GroundPoint const cached{0,0,29.58867f};
    GroundPoint const executing{2.5f,0,26.66846f};
    GroundPoint source{};
    std::size_t reads = 0;
    REQUIRE(Movement::SelectPhysicalSplineSource(cached,false,false,false,
        [&] { ++reads; return executing; },source));
    CHECK(reads == 1);
    CHECK(source.x == executing.x);
    CHECK(source.z == executing.z);
    CHECK(cached.x == 0); // selecting/planning does not relocate or stop
    CHECK(cached.z == 29.58867f);

    GroundHuntFixture path;
    path.Points = {executing,{5,0,26.66846f}};
    GroundPoints raw, grounded;
    REQUIRE(Movement::PrepareCompleteChasePath(path,source,{5,0,26.66846f},{5,0,26.66846f},
        2,false,raw,[](float,float,float) { return true; },clearGround));
    CHECK(path.CalculatedFrom.x == executing.x);
    CHECK(path.CalculatedFrom.z == executing.z);
    bool first = true;
    REQUIRE(Movement::PrepareGroundedPath(raw,source,grounded,
        [&](GroundPoint const& p) -> std::optional<float>
        {
            if (first) { CHECK(p.x == executing.x); CHECK(p.z == executing.z); first = false; }
            return executing.z;
        },clearGroundBody,inGroundBounds,installedGround));
    CHECK(grounded.front().x == executing.x);
    CHECK(grounded.front().z == executing.z);
    CHECK(grounded.back().x == 5);
    // A stale cached-source route cannot be published for this same spline.
    CHECK_FALSE(Movement::PrepareGroundedPath(raw,cached,grounded,
        flatGround,clearGround,inGroundBounds,installedGround));
    CHECK(grounded.empty());
}

TEST_CASE("Recovery navigation source selection follows spline launch state without transport projection", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint const cached{1,2,3}, live{4,5,6};
    GroundPoint source{};
    std::size_t reads = 0;
    auto current = [&] { ++reads; return live; };
    REQUIRE(Movement::SelectPhysicalSplineSource(cached,false,true,false,current,source));
    CHECK(reads == 0);
    CHECK(source.x == cached.x);
    // As in Launch, an old transport spline does not provide a world-space
    // source after the actor has left that transport.
    REQUIRE(Movement::SelectPhysicalSplineSource(cached,false,false,true,current,source));
    CHECK(reads == 0);
    CHECK(source.x == cached.x);
    CHECK_FALSE(Movement::SelectPhysicalSplineSource(cached,true,false,true,current,source));
    CHECK(reads == 0);
    GroundPoint invalid = live;
    invalid.z = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Movement::SelectPhysicalSplineSource(cached,false,false,false,[&] { return invalid; },source));
    CHECK_FALSE(Movement::SelectPhysicalSplineSource(invalid,false,true,false,current,source));
    CHECK(reads == 0);
}

TEST_CASE("Recovery navigation grounds the actual shortened hunt endpoint through continuous support", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundHuntFixture fixture;
    GroundPoint actual{0,0,29.58867f};
    GroundPoints raw, output;
    REQUIRE(Movement::PrepareCompleteChasePath(fixture,actual,{4,0,29.58867f},{4,0,29.58867f},2,true,raw,
        [](float,float,float) { return true; },clearGround));
    REQUIRE(fixture.ShortenedOnce);
    REQUIRE(raw.back().z == Approx(29.58867f));
    // Both original mesh endpoints are supported, but the shortened chord
    // ends in a 2.92021-yard hollow. This is the recorded height discrepancy.
    auto floor = [](GroundPoint const& p)
    { return 29.58867f-1.460105f*(p.x <= 2 ? p.x : 4-p.x); };
    CHECK(raw.back().z-floor(raw.back()) == Approx(2.92021f).margin(0.0001f));
    auto height = [&](GroundPoint const& p) -> std::optional<float>
    {
        float z = floor(p);
        if (z > p.z+0.8f || z < p.z-3) return std::nullopt;
        return z;
    };
    REQUIRE(Movement::PrepareGroundedPath(raw,actual,output,height,clearGroundBody,inGroundBounds,installedGround));
    REQUIRE(output.size() == 5);
    CHECK(output.front().z == actual.z); // never relocate/project the actor
    CHECK(output.back().x == raw.back().x);
    CHECK(output.back().z == Approx(26.66846f));
    CHECK(output.back().z != raw.back().z);
    for (auto const& p : output) CHECK(p.z == Approx(floor(p)));
}

TEST_CASE("Recovery navigation keeps interrupted grounded hunt splines supported between controls", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint actual{0,0,29.58867f};
    GroundPoints raw{{0,0,29.58867f},{4,0,29.58867f}}, output;
    auto floor = [](GroundPoint const& p) -> std::optional<float>
    { return 29.58867f-1.460105f*(p.x <= 2 ? p.x : 4-p.x); };
    REQUIRE(Movement::PrepareGroundedPath(raw,actual,output,floor,clearGroundBody,inGroundBounds,installedGround));
    REQUIRE(output.size() == 9);
    // StopMoving may halt at any point of the executed linear spline. The
    // old four-yard chord would be 2.92 yards above the floor at its centre.
    for (std::size_t i = 1; i < output.size(); ++i)
        for (float t : {0.1f,0.5f,0.9f})
        {
            auto const& a = output[i-1]; auto const& b = output[i];
            GroundPoint stopped{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};
            CHECK(std::abs(stopped.z-*floor(stopped)) <= 0.75f);
        }
}

TEST_CASE("Recovery navigation refuses an already unsupported hunt source without moving it", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint actual{-9827.092f,-535.2559f,29.58867f};
    GroundPoints raw{actual,{actual.x+4,actual.y,26.66846f}}, output{{1,2,3}};
    auto floor = [](GroundPoint const&) -> std::optional<float> { return 26.66846f; };
    char const* reason = "NONE";
    REQUIRE_FALSE(Movement::PrepareGroundedPath(raw,actual,output,floor,clearGroundBody,inGroundBounds,installedGround,&reason));
    CHECK(std::string(reason) == "GROUND_PATH_START_HEIGHT");
    CHECK(output.empty());
    CHECK(actual.z == 29.58867f);
    // A legitimate hover offset is part of the callback's foot support;
    // accepting it does not widen the physical support tolerance.
    GroundPoints hovering{actual,{actual.x+4,actual.y,29.58867f}};
    auto hoverFloor = [](GroundPoint const&) -> std::optional<float> { return 26.66846f+2.92021f; };
    CHECK(Movement::PrepareGroundedPath(hovering,actual,output,hoverFloor,clearGroundBody,inGroundBounds,installedGround));
}

TEST_CASE("Recovery navigation grounded hunt refuses different floors cliffs holes tiles and boundaries", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint actual{0,0,0};
    GroundPoints raw{{0,0,0},{4,0,0}}, output;
    char const* reason = "NONE";
    SECTION("other storey")
    {
        auto floor = [](GroundPoint const& p) -> std::optional<float> { return p.x < 2 ? 0.0f : 8.0f; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,floor,clearGroundBody,inGroundBounds,installedGround,&reason));
        CHECK(std::string(reason) == "GROUND_PATH_CLIFF");
    }
    SECTION("cliff")
    {
        auto floor = [](GroundPoint const& p) -> std::optional<float> { return p.x < 2 ? 0.0f : -2.0f; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,floor,clearGroundBody,inGroundBounds,installedGround,&reason));
        CHECK(std::string(reason) == "GROUND_PATH_CLIFF");
    }
    SECTION("floor hole")
    {
        auto floor = [](GroundPoint const& p) -> std::optional<float>
        { if (p.x == 2) return std::nullopt; return 0.0f; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,floor,clearGroundBody,inGroundBounds,installedGround,&reason));
        CHECK(std::string(reason) == "GROUND_PATH_HEIGHT");
    }
    SECTION("missing tile between valid endpoints")
    {
        auto tiles = [](GroundPoint const& p) { return p.x < 1.5f || p.x > 2.5f; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearGroundBody,inGroundBounds,tiles,&reason));
        CHECK(std::string(reason) == "GROUND_PATH_MISSING_TILE");
    }
    SECTION("zone crossing")
    {
        auto zone = [](GroundPoint const& p) { return p.x < 1.5f || p.x > 2.5f; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearGroundBody,zone,installedGround,&reason));
        CHECK(std::string(reason) == "GROUND_PATH_BOUNDS");
    }
    SECTION("vertical offmesh link")
    {
        raw[1] = {0,0,3};
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearGroundBody,inGroundBounds,installedGround,&reason));
        CHECK(std::string(reason) == "GROUND_PATH_VERTICAL");
    }
    CHECK(output.empty());
}

TEST_CASE("Recovery navigation grounded hunts retain body clearance for walls and floors", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint actual{0,0,0};
    GroundPoints raw{{0,0,0},{4,0,0}}, output;
    bool side = false;
    SECTION("side wall") { side = true; }
    SECTION("low beam or separating floor") { side = false; }
    auto clearBody = [&](GroundPoint const& a, GroundPoint const& b)
    {
        return LivingSurfaceCorridor::BodyClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z},0.5f,2,
            [&](ActionPosition const& from,ActionPosition const& to)
            {
                if (side) return !(from.Y > 0.4f && to.Y > 0.4f && from.X < 2 && to.X >= 2);
                return !(from.X >= 2 && from.X == to.X && from.Z < 1.3f && to.Z > 1.3f);
            });
    };
    char const* reason = "NONE";
    CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearBody,inGroundBounds,installedGround,&reason));
    CHECK(std::string(reason) == "GROUND_PATH_OBSTACLE");
    CHECK(output.empty());
}

TEST_CASE("Recovery navigation can leave a supported slope endpoint in either checked direction", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoints raw{{0,0,0},{4,0,2}}, output;
    std::size_t expectedSamples = 9;
    SECTION("ordinary path") { }
    SECTION("repeated mesh vertices")
    { raw = {{0,0,0},{0,0,0},{2,0,1},{2,0,1},{4,0,2},{4,0,2}}; }
    SECTION("nearby mesh vertex retains its real travel direction")
    { raw = {{0,0,0},{0.0005f,0,0.00025f},{4,0,2}}; expectedSamples = 10; }
    auto floor = [](GroundPoint const& p) -> std::optional<float> { return 0.5f*p.x; };
    auto clearBody = [](GroundPoint const& a, GroundPoint const& b)
    {
        return LivingSurfaceCorridor::BodyClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z},1,2,
            [](ActionPosition const& from, ActionPosition const& to)
            {
                // On this linear floor, a ray is clear exactly when neither
                // endpoint lies underground. The whole ray then stays clear.
                return from.Z >= 0.5f*from.X && to.Z >= 0.5f*to.X;
            });
    };
    // A zero-length sweep selects fixed X lateral tracks. Its uphill track
    // keeps the centre's Z and lies inside the floor, despite a clear slope.
    CHECK_FALSE(clearBody(raw.front(), raw.front()));
    REQUIRE(clearBody(raw.front(), raw.back()));
    REQUIRE(Movement::PrepareGroundedPath(raw,raw.front(),output,floor,clearBody,inGroundBounds,installedGround));
    REQUIRE(output.size() == expectedSamples);
    auto reached = output.back();
    CHECK(reached.x == 4);
    CHECK(reached.z == 2);
    GroundPoints reverse{reached,raw.front()}, returned;
    CHECK_FALSE(clearBody(reached,reached));
    REQUIRE(Movement::PrepareGroundedPath(reverse,reached,returned,floor,clearBody,inGroundBounds,installedGround));
    REQUIRE(returned.size() == 9);
    CHECK(returned.front().x == reached.x);
    CHECK(returned.front().z == reached.z); // Never relocate or project the source.
    CHECK(returned.back().x == 0);
    CHECK(returned.back().z == 0);
}

TEST_CASE("Recovery body sweeps preserve finite direction for subnormal displacements", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    ActionPosition from{0,0,0,0};
    float tiny = std::numeric_limits<float>::denorm_min();
    ActionPosition to{0,tiny,tiny,0};
    unsigned rays = 0;
    REQUIRE(LivingSurfaceCorridor::BodyClear(from,to,1,2,
        [&](ActionPosition const& a, ActionPosition const& b)
        {
            ++rays;
            CHECK(LivingSurfaceCorridor::Finite(a));
            CHECK(LivingSurfaceCorridor::Finite(b));
            // The diagonal edge's lateral direction remains normalized even
            // when a float hypot would round its length to one subnormal unit.
            CHECK((std::hypot(a.X,a.Y) == Approx(0.0f).margin(0.00001f) ||
                std::hypot(a.X,a.Y) == Approx(1.0f).margin(0.00001f)));
            return true;
        }));
    CHECK(rays == 15);
}

TEST_CASE("Recovery navigation first movement segment still rejects obstructed source bodies", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoints raw{{0,0,0},{4,0,0}}, output;
    bool side = false;
    SECTION("source side wall") { side = true; }
    SECTION("source overhead obstacle") { side = false; }
    unsigned bodyCalls = 0;
    auto clearBody = [&](GroundPoint const& a, GroundPoint const& b)
    {
        ++bodyCalls;
        CHECK(a.x == 0);
        CHECK(b.x == 0.5f); // The actual first half-yard sweep includes its source.
        return LivingSurfaceCorridor::BodyClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z},0.5f,2,
            [&](ActionPosition const& from, ActionPosition const& to)
            {
                if (side) return !(from.X == 0 && from.Y > 0.4f);
                return !(from.X == 0 && to.X == 0 && from.Z < 1.3f && to.Z > 1.3f);
            });
    };
    char const* reason = "NONE";
    CHECK_FALSE(Movement::PrepareGroundedPath(raw,raw.front(),output,flatGround,clearBody,inGroundBounds,installedGround,&reason));
    CHECK(std::string(reason) == "GROUND_PATH_OBSTACLE");
    CHECK(bodyCalls == 1);
    CHECK(output.empty());
}

TEST_CASE("Recovery navigation grounded hunt work and physical lengths remain bounded", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint actual{0,0,0};
    GroundPoints raw{{0,0,0},{63.5f,0,0}}, output;
    unsigned heights = 0, tiles = 0, body = 0, rays = 0;
    auto height = [&](GroundPoint const&) -> std::optional<float> { ++heights; return 0.0f; };
    auto tile = [&](GroundPoint const&) { ++tiles; return true; };
    auto clearBody = [&](GroundPoint const& a, GroundPoint const& b)
    {
        ++body;
        return LivingSurfaceCorridor::BodyClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z},0.5f,2,
            [&](auto const&,auto const&) { ++rays; return true; });
    };
    SECTION("maximum supported batch")
    {
        REQUIRE(Movement::PrepareGroundedPath(raw,actual,output,height,clearBody,inGroundBounds,tile));
        CHECK(output.size() == 128);
        CHECK(heights == 128);
        CHECK(tiles == 128);
        CHECK(body == 127);
        CHECK(rays == 1905);
    }
    SECTION("sample limit before callbacks")
    {
        raw[1].x = 64;
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,height,clearBody,inGroundBounds,tile));
        CHECK(heights == 0); CHECK(tiles == 0); CHECK(body == 0);
    }
    SECTION("three dimensional raw range before callbacks")
    {
        raw[1] = {60,0,30};
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,height,clearBody,inGroundBounds,tile));
        CHECK(heights == 0); CHECK(tiles == 0); CHECK(body == 0);
    }
    SECTION("actual supported length may exceed the raw chord")
    {
        raw[1].x = 50;
        auto slope = [](GroundPoint const& p) -> std::optional<float> { return 1.0f*p.x; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,slope,clearBody,inGroundBounds,tile));
    }
    CHECK(heights <= 128); CHECK(tiles <= 128); CHECK(body <= 127); CHECK(rays <= 1905);
}

TEST_CASE("Recovery navigation grounded hunt preserves graph corners and rejects malformed source geometry", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoint actual{0,0,0};
    GroundPoints raw{{0,0,0},{2,0,0},{2,2,0}}, output;
    SECTION("corners")
    {
        REQUIRE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearGroundBody,inGroundBounds,installedGround));
        REQUIRE(output.size() == 9);
        CHECK(output[4].x == 2); CHECK(output[4].y == 0);
        for (std::size_t i = 1; i < output.size(); ++i)
            CHECK(std::hypot(output[i].x-output[i-1].x,output[i].y-output[i-1].y) <= 0.5f);
    }
    SECTION("source changed")
    {
        actual.z = 0.01f;
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearGroundBody,inGroundBounds,installedGround));
        CHECK(output.empty());
    }
    SECTION("nonfinite coordinate")
    {
        raw[1].z = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,flatGround,clearGroundBody,inGroundBounds,installedGround));
        CHECK(output.empty());
    }
    SECTION("only repeated vertices do not produce a movement route")
    {
        raw = {actual,actual,actual};
        unsigned calls = 0;
        auto height = [&](GroundPoint const&) -> std::optional<float> { ++calls; return 0.0f; };
        CHECK_FALSE(Movement::PrepareGroundedPath(raw,actual,output,height,clearGroundBody,inGroundBounds,installedGround));
        CHECK(output.empty());
        CHECK(calls == 0);
    }
}

TEST_CASE("Recovery navigation bypasses ground support only for a wholly proved swimming route", "[AIWorld][Movement][HuntPath][GroundedPath]")
{
    GroundPoints path{{0,0,4},{4,0,4}};
    unsigned probes = 0;
    SECTION("all sampled points swimming")
    {
        CHECK(Movement::FullySwimmingPath(path,[&](GroundPoint const&) { ++probes; return true; }));
        CHECK(probes == 5);
    }
    SECTION("dry start despite swimming target")
    {
        CHECK_FALSE(Movement::FullySwimmingPath(path,[](GroundPoint const& p) { return p.x > 0; }));
    }
    SECTION("dry end despite swimming source")
    {
        CHECK_FALSE(Movement::FullySwimmingPath(path,[](GroundPoint const& p) { return p.x < 4; }));
    }
    SECTION("dry segment between swimming endpoints")
    {
        CHECK_FALSE(Movement::FullySwimmingPath(path,[](GroundPoint const& p) { return p.x < 1.5f || p.x > 2.5f; }));
    }
    SECTION("invalid or excessive swimming work")
    {
        path[1].x = 513;
        CHECK_FALSE(Movement::FullySwimmingPath(path,[&](GroundPoint const&) { ++probes; return true; }));
        CHECK(probes == 1); // the actual source, then the old 512-sample gate
    }
}
