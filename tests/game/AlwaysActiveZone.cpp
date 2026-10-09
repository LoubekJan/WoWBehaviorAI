#include "AlwaysActiveZone.h"
#include <catch2/catch.hpp>
#include <limits>
#include <set>

namespace
{
float GridCenter(uint32 grid)
{
    return (float(grid) + 0.5f - CENTER_GRID_ID) * SIZE_OF_GRIDS;
}
}

TEST_CASE("Always-active zone rejects empty or corrupt census coordinates", "[maps][elwynn]")
{
    AlwaysActiveZoneCoverage coverage;
    CHECK(coverage.GetGrids().empty());
    CHECK_FALSE(coverage.AddSpawn(std::numeric_limits<float>::quiet_NaN(), 0));
    CHECK_FALSE(coverage.AddSpawn(0, std::numeric_limits<float>::infinity()));
    CHECK_FALSE(coverage.AddSpawn(MAP_SIZE, 0));
    CHECK(coverage.GetGrids().empty());
}

TEST_CASE("Always-active zone fills empty interior and adds movement buffer", "[maps][elwynn]")
{
    AlwaysActiveZoneCoverage coverage;
    REQUIRE(coverage.AddSpawn(GridCenter(12), GridCenter(28)));
    REQUIRE(coverage.AddSpawn(GridCenter(15), GridCenter(31)));
    REQUIRE(coverage.AddSpawn(GridCenter(12), GridCenter(28)));
    auto grids = coverage.GetGrids();
    REQUIRE(grids.size() == 36);
    std::set<uint32> unique;
    for (GridCoord const& grid : grids)
    {
        CHECK(grid.IsCoordValid());
        CHECK(grid.x_coord >= 11);
        CHECK(grid.x_coord <= 16);
        CHECK(grid.y_coord >= 27);
        CHECK(grid.y_coord <= 32);
        unique.insert(grid.GetId());
    }
    CHECK(unique.size() == grids.size());
    CHECK(unique.count(GridCoord(14, 30).GetId()) == 1);
}

TEST_CASE("Always-active zone clamps padding to map edges", "[maps][elwynn]")
{
    AlwaysActiveZoneCoverage coverage;
    REQUIRE(coverage.AddSpawn(GridCenter(0), GridCenter(63)));
    auto grids = coverage.GetGrids();
    REQUIRE(grids.size() == 4);
    for (GridCoord const& grid : grids)
    {
        CHECK(grid.IsCoordValid());
        CHECK(grid.x_coord <= 1);
        CHECK(grid.y_coord >= 62);
    }
}

TEST_CASE("Bounded lab footprint activates one grid without neighboring terrain", "[maps][scope]")
{
    AlwaysActiveZoneCoverage coverage;
    REQUIRE(coverage.AddBounds(166.667f, 366.667f, 700.0f, 900.0f));
    auto grids = coverage.GetGrids(0);
    REQUIRE(grids.size() == 1);
    CHECK(grids.front() == Trinity::ComputeGridCoord(266.667f, 800.0f));
    // The existing zone loader continues to use its original padding.
    CHECK(coverage.GetGrids().size() == 9);
}

TEST_CASE("Bounded coverage rejects corrupt and inverted rectangles", "[maps][scope]")
{
    AlwaysActiveZoneCoverage coverage;
    CHECK_FALSE(coverage.AddBounds(20, 10, 0, 10));
    CHECK_FALSE(coverage.AddBounds(10, 10, 0, 10));
    CHECK_FALSE(coverage.AddBounds(0, 10, 10, 0));
    CHECK_FALSE(coverage.AddBounds(0, std::numeric_limits<float>::infinity(), 0, 10));
    CHECK_FALSE(coverage.AddBounds(std::numeric_limits<float>::quiet_NaN(), 10, 0, 10));
    CHECK_FALSE(coverage.AddBounds(0, MAP_SIZE, 0, 10));
    CHECK(coverage.GetGrids(0).empty());
}

TEST_CASE("Bounded coverage includes every intersecting grid without padding", "[maps][scope]")
{
    AlwaysActiveZoneCoverage coverage;
    REQUIRE(coverage.AddBounds(400, 700, 700, 900));
    auto grids = coverage.GetGrids(0);
    REQUIRE(grids.size() == 2);
    for (GridCoord const& grid : grids)
        CHECK(grid.y_coord == Trinity::ComputeGridCoord(400, 800).y_coord);
}
