#include "tc_catch2.h"
#include "Simulation/SimulationPathBounds.h"
#include <limits>
#include <vector>

namespace
{
    struct Point { float x, y, z; };
    SimulationScope LabScope()
    {
        SimulationScope scope;
        scope.MapId = 725;
        scope.ZoneIds = {4988};
        scope.Bounds = SimulationBounds{166.667f, 366.667f, 700, 900};
        return scope;
    }
}

TEST_CASE("Scoped escape proves every route segment on the custom map", "[aiworld][scope][movement]")
{
    auto scope = LabScope();
    auto zone = [](float, float, float) { return 4988u; };
    std::vector<Point> route{{266.667f, 800, 0}, {294.667f, 811, 0}};
    CHECK(Movement::PathWithinSimulationScope(route, 725, scope, zone));
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 0, scope, zone));
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 725, scope,
        [](float, float, float) { return 12u; }));
    // Both corners have the expected zone; the corridor crosses another zone.
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 725, scope,
        [](float x, float, float) { return x > 276 && x < 283 ? 4990u : 4988u; }));
    route.back().x = 367;
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 725, scope, zone));
    route.back().x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 725, scope, zone));
    route.clear();
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 725, scope, zone));
}

TEST_CASE("Scoped escape fails closed and preserves default Elwynn zone membership", "[aiworld][scope][movement]")
{
    SimulationScope scope;
    std::vector<Point> route{{-9500, -200, 60}, {-9490, -195, 60}};
    auto zone = [](float, float, float) { return 12u; };
    CHECK(Movement::PathWithinSimulationScope(route, 0, scope, zone));
    scope.ZoneIds.clear();
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 0, scope, zone));
    scope = LabScope();
    scope.Bounds->MinX = scope.Bounds->MaxX;
    CHECK_FALSE(Movement::PathWithinSimulationScope(route, 725, scope, zone));
}
