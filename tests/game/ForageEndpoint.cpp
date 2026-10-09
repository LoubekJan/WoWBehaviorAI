/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingForageGround.h"
#include "Agent/LivingForagePolicy.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <limits>

namespace
{
    using Status = LivingSurfaceCorridor::Status;

    template<class Height, class Clear>
    Status Finish(LivingForageGroundSearch& search, ActionPosition const& from, ActionPosition const& target,
        Height&& height, Clear&& clear, bool prefix)
    {
        for (unsigned turn = 0; turn < 6; ++turn)
        {
            auto status = search.Advance(from, target, height, clear, prefix);
            if (status != Status::Pending) return status;
        }
        FAIL("A sixteen-yard forage ray must finish within five admitted slices");
        return Status::Pending;
    }
}

TEST_CASE("Forage keeps a supported local prefix before an unavailable radial endpoint", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0}, target{0,8,0,0};
    auto height = [](ActionPosition const& p) -> std::optional<float>
    { return p.X <= 5.25f ? std::optional(0.0f) : std::nullopt; };
    auto clear = [](auto const&, auto const&) { return true; };
    LivingForageGroundSearch exact, prefix;
    REQUIRE(Finish(exact, from, target, height, clear, false) == Status::Rejected);
    REQUIRE_FALSE(exact.Resolved.has_value());
    REQUIRE(Finish(prefix, from, target, height, clear, true) == Status::Complete);
    REQUIRE(prefix.Resolved.has_value());
    CHECK(prefix.Resolved->X == 5.0f);
    CHECK(prefix.Resolved->Z == 0.0f);
    CHECK(prefix.Surface.State == Status::Rejected); // No claim that the full radial route exists.
}

TEST_CASE("Forage connected-prefix mode does not select a nearby disconnected shelf", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0.5f}, target{0,8,0,0.5f};
    auto height = [](ActionPosition const& p) -> std::optional<float>
    { return p.X == 8 ? 2.5f : 0.5f; };
    auto wall = [](ActionPosition const&, ActionPosition const& b) { return b.X <= 4.25f; };
    LivingForageGroundSearch exact, prefix;
    REQUIRE(Finish(exact, from, target, height, wall, false) == Status::Complete);
    CHECK(exact.Resolved->X == 8);
    CHECK(exact.Resolved->Z == 2.5f);
    REQUIRE(Finish(prefix, from, target, height, wall, true) == Status::Complete);
    CHECK(prefix.Resolved->X == 4);
    CHECK(prefix.Resolved->Z == from.Z); // Keep the actor's floor, including hover exactly once.
}

TEST_CASE("Every short forage direction may overshoot while still having a useful supported prefix", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0};
    auto height = [](ActionPosition const& p) -> std::optional<float>
    { return std::hypot(p.X, p.Y) <= 3.75f ? std::optional(0.0f) : std::nullopt; };
    unsigned offered = 0;
    for (uint32 leg = 0; leg < 6; ++leg)
    {
        auto target = LivingForagePolicy::LocalWaypoint(from, 80992, leg, 2);
        REQUIRE_FALSE(height(target).has_value()); // Even the four-yard endpoint is too far.
        LivingForageGroundSearch prefix;
        REQUIRE(Finish(prefix, from, target, height, [](auto const&, auto const&) { return true; }, true) == Status::Complete);
        REQUIRE(prefix.Resolved.has_value());
        auto distance = std::hypot(prefix.Resolved->X, prefix.Resolved->Y);
        CHECK(distance > 3.0f); // Outside the runtime's existing reached radius.
        CHECK(distance <= 3.75f);
        ++offered;
    }
    REQUIRE(offered == 6);
}

TEST_CASE("Forage prefix resolution retains workload and separate navigation admission bounds", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0.5f}, target{0,16,0,0.5f};
    LivingForageGroundSearch prefix;
    unsigned turns = 0, heights = 0, segments = 0;
    float previousX = 0;
    while (prefix.State == Status::Pending)
    {
        REQUIRE(++turns <= 5);
        unsigned sliceHeights = 0, sliceSegments = 0;
        PlanningWorkBudget budget(std::chrono::seconds(1), 1);
        PlanningWorkBudget::Scope scope(budget);
        auto permit = PlanningWorkBudget::TryAcquire();
        REQUIRE(bool(permit));
        prefix.Advance(from, target, [&](ActionPosition const& p) -> std::optional<float>
        {
            ++sliceHeights; ++heights;
            if (p.X > 13.75f) return std::nullopt;
            float floor = 0.5f + p.X * 0.5f;
            return floor <= p.Z + 0.8f ? std::optional(floor) : std::nullopt;
        }, [&](ActionPosition const& a, ActionPosition const& b)
        {
            ++sliceSegments; ++segments;
            CHECK(a.X == previousX); // Resumption never repeats prior geometry work.
            previousX = b.X;
            return true;
        }, true);
        permit.Finish();
        CHECK(sliceHeights <= 8);
        CHECK(sliceSegments <= 8);
        REQUIRE_FALSE(bool(PlanningWorkBudget::TryAcquire())); // No navmesh/execution work in this permit.
    }
    REQUIRE(prefix.State == Status::Complete);
    REQUIRE(prefix.Resolved.has_value());
    CHECK(prefix.Resolved->X == 13.5f);
    CHECK(prefix.Resolved->Z == 7.25f);
    CHECK(turns == 4);
    CHECK(heights == 29);
    CHECK(segments == 27);
}

TEST_CASE("Forage prefixes reject unsupported origins and movement inside the reached radius", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0}, target{0,8,0,0};
    auto clear = [](auto const&, auto const&) { return true; };
    SECTION("Invalid starting floor")
    {
        LivingForageGroundSearch prefix;
        CHECK(Finish(prefix, from, target, [](auto const&) { return std::optional(2.0f); }, clear, true) == Status::Rejected);
        CHECK_FALSE(prefix.Resolved.has_value());
    }
    SECTION("Cliff after three yards")
    {
        LivingForageGroundSearch prefix;
        CHECK(Finish(prefix, from, target, [](ActionPosition const& p) { return std::optional(p.X <= 3 ? 0.0f : -5.0f); }, clear, true) == Status::Rejected);
        CHECK_FALSE(prefix.Resolved.has_value());
    }
    SECTION("Cliff after five yards")
    {
        LivingForageGroundSearch prefix;
        REQUIRE(Finish(prefix, from, target, [](ActionPosition const& p) { return std::optional(p.X <= 5 ? 0.0f : -5.0f); }, clear, true) == Status::Complete);
        REQUIRE(prefix.Resolved.has_value());
        CHECK(prefix.Resolved->X == 5);
        CHECK(prefix.Resolved->Z == 0);
    }
    SECTION("Wall at the origin")
    {
        LivingForageGroundSearch prefix;
        CHECK(Finish(prefix, from, target, [](auto const&) { return std::optional(0.0f); },
            [](auto const&, auto const&) { return false; }, true) == Status::Rejected);
        CHECK_FALSE(prefix.Resolved.has_value());
    }
}

TEST_CASE("Forage prefix cursors reject changed physical or query context", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0}, target{0,16,0,0};
    auto height = [](auto const&) { return std::optional(0.0f); };
    auto clear = [](auto const&, auto const&) { return true; };
    LivingForageGroundSearch prefix;
    REQUIRE(prefix.Advance(from, target, height, clear, true) == Status::Pending);
    REQUIRE(prefix.Supported.has_value());
    bool mode = true;
    SECTION("Origin moved") { from.X = 1; }
    SECTION("Target moved") { target.Y = 1; }
    SECTION("Map changed") { from.MapId = 1; }
    SECTION("Resolution mode changed") { mode = false; }
    REQUIRE(prefix.Advance(from, target, height, clear, mode) == Status::Rejected);
    CHECK_FALSE(prefix.Resolved.has_value());
    CHECK_FALSE(prefix.Supported.has_value());
}

TEST_CASE("Invalid forage prefix requests issue no geometry queries", "[AIWorld][ForageEndpoint]")
{
    ActionPosition from{0,0,0,0}, target{0,8,0,0};
    SECTION("Nonfinite source") { from.Z = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Nonfinite target") { target.X = std::numeric_limits<float>::infinity(); }
    SECTION("Map mismatch") { target.MapId = 1; }
    SECTION("Beyond local range") { target.X = 17; }
    unsigned calls = 0;
    LivingForageGroundSearch prefix;
    REQUIRE(prefix.Advance(from, target, [&](auto const&) { ++calls; return std::optional(0.0f); },
        [&](auto const&, auto const&) { ++calls; return true; }, true) == Status::Rejected);
    CHECK(calls == 0);
    CHECK_FALSE(prefix.Resolved.has_value());
}
