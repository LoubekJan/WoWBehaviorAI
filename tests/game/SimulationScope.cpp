#include "tc_catch2.h"
#include "Simulation/SimulationScope.h"
#include <limits>

TEST_CASE("Simulation scope preserves default root-zone membership", "[aiworld][scope]")
{
    SimulationScope scope;
    REQUIRE(scope.Valid());
    CHECK(scope.Contains(0, 12, -9500, -200, 60));
    CHECK_FALSE(scope.ContainsMapZone(725, 12));
    CHECK_FALSE(scope.ContainsMapZone(0, 13));
    CHECK(scope.ContainsActor(0, 80335));
}

TEST_CASE("Lab scope checks map zone bounds and explicit spawn identity", "[aiworld][scope]")
{
    SimulationScope scope;
    scope.MapId = 725; scope.ZoneIds = {4988};
    scope.Bounds = SimulationBounds{166.667f, 366.667f, 700, 900};
    scope.SpawnIds = {900725};
    REQUIRE(scope.Valid());
    CHECK(scope.Contains(725, 4988, 266.667f, 800, 0));
    CHECK(scope.Contains(725, 4988, 166.667f, 900, 0));
    CHECK_FALSE(scope.Contains(0, 4988, 266.667f, 800, 0));
    CHECK_FALSE(scope.Contains(725, 12, 266.667f, 800, 0));
    CHECK_FALSE(scope.Contains(725, 4988, 366.668f, 800, 0));
    CHECK_FALSE(scope.Contains(725, 4988, 266.667f, 699.999f, 0));
    CHECK(scope.ContainsActor(725, 900725));
    CHECK_FALSE(scope.ContainsActor(0, 900725));
    CHECK_FALSE(scope.ContainsActor(725, 900726));
    // Membership survives a live excursion, allowing diagnostics to retain it.
    CHECK_FALSE(scope.ContainsPosition(725, 500, 800, 0));
    CHECK(scope.ContainsActor(725, 900725));
}

TEST_CASE("Invalid simulation scopes and nonfinite positions fail closed", "[aiworld][scope]")
{
    SimulationScope scope;
    auto nan = std::numeric_limits<float>::quiet_NaN();
    auto inf = std::numeric_limits<float>::infinity();
    CHECK_FALSE(scope.ContainsPosition(0, nan, 0, 0));
    CHECK_FALSE(scope.ContainsPosition(0, 0, inf, 0));
    CHECK_FALSE(scope.ContainsPosition(0, 0, 0, nan));
    scope.ZoneIds.clear(); CHECK_FALSE(scope.Valid()); CHECK_FALSE(scope.ContainsActor(0, 1));
    scope.ZoneIds = {12, 12}; CHECK_FALSE(scope.Valid());
    scope.ZoneIds = {0}; CHECK_FALSE(scope.Valid());
    scope.ZoneIds = {12}; scope.Bounds = SimulationBounds{0, 0, 0, 1}; CHECK_FALSE(scope.Valid());
    scope.Bounds = SimulationBounds{0, 1, 0, inf}; CHECK_FALSE(scope.Valid());
    scope.Bounds.reset(); scope.SpawnIds = {1, 1}; CHECK_FALSE(scope.Valid());
    scope.SpawnIds = {0}; CHECK_FALSE(scope.Valid());
}
