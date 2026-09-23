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
