/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Inference/RecoveryAdvice.h"
#include "Agent/LivingAdviceState.h"
#include "Agent/LivingRefugePolicy.h"
#include "Agent/LivingAdviceBudget.h"
#include "Agent/LivingMovementWatchdog.h"
#include "Agent/LivingForagePolicy.h"
#include "Agent/LivingEscapeProgress.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <limits>

TEST_CASE("Recovery refuge waits for failure then reserves a real home retry window", "[AIWorld][RecoveryNavigation]")
{
    LivingRefugeState s;
    ActionPosition here{0,50,0,0}, home{0,0,0,0};
    CHECK_FALSE(s.Begin(300999, 1000, 1000, here, home, 96));
    CHECK_FALSE(s.Begin(301000, 1000, 300000, here, home, 96));
    REQUIRE(s.Begin(301000, 1000, 1000, here, home, 96));
    CHECK(s.Active(420999));
    CHECK_FALSE(s.Active(421000));
    CHECK_FALSE(s.Begin(421000, 1000, 1000, here, home, 96));
    REQUIRE(s.Begin(481000, 1000, 1000, here, home, 96));
    CHECK(s.Episodes == 2);
}

TEST_CASE("Recovery refuge never shifts its anchor or expands original home boundary", "[AIWorld][RecoveryNavigation]")
{
    LivingRefugeState s;
    ActionPosition home{0,0,0,0};
    REQUIRE(s.Begin(301000, 1000, 1000, {0,90,0,0}, home, 96));
    CHECK(s.Contains({0,95,0,0}));
    CHECK_FALSE(s.Contains({0,97,0,0}));
    CHECK_FALSE(s.Contains({0,78,1,0}));
    CHECK_FALSE(s.Contains({1,90,0,0}));
    REQUIRE(s.Begin(481000, 1000, 1000, {0,95,0,0}, home, 150));
    CHECK(s.Anchor->X == 90);
    CHECK(s.HomeLimit == 96);
    CHECK_FALSE(s.Begin(661000, 1000, 1000, {0,110,0,0}, home, 150));
    CHECK_FALSE(s.Contains({0,90,0,std::numeric_limits<float>::infinity()}));
    s = {}; // Real arrival or rematerialization clears the temporary episode.
    CHECK_FALSE(s.Active(700000));
    CHECK_FALSE(s.Contains(home));
}

TEST_CASE("Recovery navigation crosses a continuous hollow without cutting terrain", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    // The surface is two yards below the endpoint chord; the old interpolation
    // rejected its middle despite every half-yard step being walkable.
    auto ground = [](auto const& p) -> std::optional<float> { return 10.0f - std::min(p.X, 6.0f-p.X) * (2.0f/3.0f); };
    auto clear = [](auto const&, auto const&) { return true; };
    NavigationDiagnostics nav;
    char const* reason = "NONE";
    auto path = SurfaceConnector({0,0,0,11.58f}, {0,6,0,10}, ground, clear, &reason, ActionPosition{0,0,0,10}, &nav);
    REQUIRE_FALSE(path.empty());
    REQUIRE(path.front().Z == Approx(11.58));
    REQUIRE(path.back().X == 6);
    REQUIRE(path.back().Z == 10);
    REQUIRE(std::min_element(path.begin(), path.end(), [](auto const& a, auto const& b) { return a.Z < b.Z; })->Z == Approx(8));
    REQUIRE(nav.ConnectorSamples == 12);
    for (size_t i = 2; i < path.size(); ++i) REQUIRE(std::abs(path[i].Z-path[i-1].Z) <= .75f);
}

TEST_CASE("Recovery navigation records the first unsafe segment and rejects another floor", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    auto clear = [](auto const&, auto const&) { return true; };
    NavigationDiagnostics nav;
    char const* reason = "NONE";
    auto cliff = [](auto const& p) -> std::optional<float> { return p.X < 2 ? 10 : 6; };
    REQUIRE(SurfaceConnector({0,0,0,10}, {0,4,0,10}, cliff, clear, &reason, {}, &nav).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_CLIFF");
    REQUIRE(nav.RejectedX == 2); REQUIRE(nav.RejectedGroundZ == 6); REQUIRE(nav.PreviousGroundZ == 10);
    auto flat = [](auto const&) -> std::optional<float> { return 10; };
    REQUIRE(SurfaceConnector({0,0,0,10}, {0,4,0,12}, flat, clear, &reason).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_END_HEIGHT");
    REQUIRE(SurfaceConnector({0,0,0,10}, {0,4,0,10}, flat,
        [](auto const&, auto const& b) { return b.X < 2; }, &reason, {}, &nav).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_OBSTACLE"); REQUIRE(nav.RejectedX == 2);
    REQUIRE(nav.ConnectorSamples == 4);
}

TEST_CASE("Recovery navigation rejoins a visited polygon then completes the home corridor", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    RouteMemory memory;
    ActionPosition stranded{0,0,0,1.58f}, joined{0,4,0,0}, corner{0,4,14,0}, home{0,0,14,0};
    memory.Remember(joined);
    // Exhaustion of unrelated exploratory backtracks must not forbid a safe
    // surface repair. It still must not permit the same repair indefinitely.
    for (unsigned i = 0; i < 16; ++i) memory.Backtracks.push_back({{0,float(i*5),40,0}, {0,float(i*5),45,0}});
    REQUIRE(memory.Allows(stranded, joined, false, true));
    auto route = SurfaceConnector(stranded, joined,
        [](auto const&) -> std::optional<float> { return 0; }, [](auto const&, auto const&) { return true; }, nullptr, joined);
    REQUIRE_FALSE(route.empty());
    memory.CommitRejoin(stranded, joined);
    REQUIRE_FALSE(memory.Allows(stranded, joined, false, true));
    ActionPosition here = route.back();
    memory.Remember(corner); memory.Remember(home);
    memory.Planned = Corridor({joined, corner, home});
    unsigned arrivals = 0;
    while (!memory.Planned.empty() && arrivals < 10)
    {
        auto next = memory.Planned.front();
        REQUIRE(memory.Allows(here, next, true, false));
        REQUIRE_FALSE(memory.Advance(here)); // merely planning is no progress
        here = next;
        REQUIRE(memory.Advance(here)); ++arrivals;
    }
    REQUIRE(Distance(here, home) < .01f);
    REQUIRE(memory.Planned.empty());
    memory.Reject(joined, corner);
    REQUIRE_FALSE(memory.Allows(joined, corner, true, false));
}

TEST_CASE("Recovery food exploration does not count a blocked route as a search", "[AIWorld][RecoveryNavigation]")
{
    LivingFoodMemory food;
    ActionPosition home{0,0,0,0}, meal{0,20,0,0};
    food.Fed(meal, 1000); food.Unreachable(meal, 2000);
    REQUIRE(food.Visits(meal, 2000) == 0);
    REQUIRE_FALSE(food.FoodHint(home, 80, 2000).has_value());
    REQUIRE(food.FoodHint(home, 80, 62000).has_value());
    bool foundLocal = false;
    for (uint32 i = 0; i < 6; ++i)
    {
        auto p = LivingForagePolicy::LocalWaypoint(home, 80992, i);
        REQUIRE(LivingReturnPolicy::Distance(home, p) >= 7.99f);
        REQUIRE(LivingReturnPolicy::Distance(home, p) <= 16.01f);
        foundLocal |= p.X > 0 && p.Y > 0; // a local open quadrant in a narrow cave
    }
    REQUIRE(foundLocal);
    food.Searched(meal, 63000);
    REQUIRE(food.Visits(meal, 63000) == 1);
    REQUIRE_FALSE(food.FoodHint(home, 80, 63000).has_value());
    food.Unreachable(meal, 64000);
    food.Fed(meal, 65000); // fresh positive evidence supersedes a failed query
    REQUIRE_FALSE(food.RecentlyBlocked(meal, 65000));
    REQUIRE(food.FoodHint(home, 80, 65000).has_value());
}

TEST_CASE("Empty food searches retain reachable short candidates on the current cave floor", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingForagePolicy;
    using namespace LivingReturnPolicy;
    // A five-yard passage reaches a corner before any old local endpoint.
    // The floor above is visible to a +4 height probe, but is not this floor.
    ActionPosition here{0, -9033.841f, -562.5976f, 55.24212f};
    constexpr float hover = 0.5f;
    float floor = here.Z - hover;
    auto inside = [&](ActionPosition const& p)
    { return p.X > here.X && p.X <= here.X + 5 && std::abs(p.Y - here.Y) < 1; };
    auto height = [&](ActionPosition const& p, float lift) -> std::optional<float>
    {
        if (!inside(p)) return std::nullopt;
        float query = p.Z - hover + lift;
        return (query >= floor + 3 ? floor + 3 : floor) + hover;
    };
    unsigned oldOptions = 0, shortOptions = 0;
    for (uint32 leg = 0; leg < 120; ++leg)
    {
        auto old = LocalWaypoint(here, 80992, leg);
        oldOptions += inside(old);
        auto candidate = LocalWaypoint(here, 80992, leg, 2);
        REQUIRE(UsefulStep(here, candidate));
        REQUIRE(Distance(here, candidate) <= Distance(here, old));
        LivingForageGroundSearch groundSearch;
        auto status = groundSearch.Advance(here, candidate,
            [&](auto const& p) { return height(p, 0.8f); }, [](auto const&, auto const&) { return true; });
        while (status == LivingSurfaceCorridor::Status::Pending)
            status = groundSearch.Advance(here, candidate,
                [&](auto const& p) { return height(p, 0.8f); }, [](auto const&, auto const&) { return true; });
        auto grounded = groundSearch.Resolved;
        if (!grounded) continue;
        ++shortOptions;
        REQUIRE(grounded->Z == here.Z); // Includes hover exactly once.
        REQUIRE(*height(candidate, 4.0f) - grounded->Z > 1.0f);
        LivingSurfaceCorridor::Search proof;
        auto supported = [&](ActionPosition const& p) -> std::optional<float>
        { return p.X == here.X && p.Y == here.Y ? std::optional(here.Z) : height(p, 0.8f); };
        REQUIRE(LivingSurfaceCorridor::Advance(proof, here, *grounded, supported,
            [](auto const&, auto const&) { return true; }, [](auto const&) { return true; }, 32) ==
            LivingSurfaceCorridor::Status::Complete);
        // Resolving a target does not grant a walk through a wall or absent tile.
        LivingSurfaceCorridor::Search wall, missingTile;
        REQUIRE(LivingSurfaceCorridor::Advance(wall, here, *grounded, supported,
            [](auto const&, auto const&) { return false; }, [](auto const&) { return true; }, 32) ==
            LivingSurfaceCorridor::Status::Rejected);
        REQUIRE(LivingSurfaceCorridor::Advance(missingTile, here, *grounded, supported,
            [](auto const&, auto const&) { return true; }, [](auto const&) { return true; }, 32,
            [](auto const&) { return false; }) == LivingSurfaceCorridor::Status::Rejected);
    }
    REQUIRE(oldOptions == 0);
    REQUIRE(shortOptions > 0);
}

TEST_CASE("Local food grounding follows a supported slope but rejects a cliff", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    ActionPosition here{0,0,0,0.5f}, target{0,8,0,here.Z};
    constexpr float hover = 0.5f;
    auto slopedHeight = [&](ActionPosition const& p) -> std::optional<float>
    {
        float floor = p.X * 0.5f;
        float query = p.Z - hover + 0.8f;
        return query >= floor && query - floor <= 3 ? std::optional(floor + hover) : std::nullopt;
    };
    LivingForageGroundSearch groundSearch;
    auto status = groundSearch.Advance(here, target, slopedHeight, [](auto const&, auto const&) { return true; });
    while (status == LivingSurfaceCorridor::Status::Pending)
        status = groundSearch.Advance(here, target, slopedHeight, [](auto const&, auto const&) { return true; });
    auto grounded = groundSearch.Resolved;
    REQUIRE(grounded.has_value());
    REQUIRE(grounded->Z == 4.5f); // Cumulative climb >3 is valid via continuous support.
    auto cliff = [&](ActionPosition const& p) -> std::optional<float>
    { return p.X <= 2 ? hover : -5 + hover; };
    LivingForageGroundSearch cliffSearch;
    REQUIRE(cliffSearch.Advance(here, target, cliff, [](auto const&, auto const&) { return true; }) ==
        LivingSurfaceCorridor::Status::Rejected);
}

TEST_CASE("Short local food candidates keep finite search memory exclusions", "[AIWorld][RecoveryAdvice]")
{
    LivingFoodMemory memory;
    ActionPosition here{0,0,0,0};
    memory.Searched(here, 1000);
    auto shortLeg = LivingForagePolicy::LocalWaypoint(here, 80992, 0, 2);
    REQUIRE(memory.Visits(shortLeg, 2000) == 1);
    // Advice still avoids circling through recent searches. Natural forage
    // uses the local candidate independently and proves its route again.
    REQUIRE(LivingReturnPolicy::UsefulStep(here, shortLeg));
    LivingForageGroundSearch groundSearch;
    REQUIRE(groundSearch.Advance(here, shortLeg, [](auto const&) { return std::optional(0.0f); },
        [](auto const&, auto const&) { return true; }) == LivingSurfaceCorridor::Status::Complete);
    REQUIRE(memory.Visits(shortLeg, 601000) == 0);
    REQUIRE_FALSE(memory.FoodHint(here, 80, 601000).has_value()); // Searching is never feeding.
    memory.Unreachable(shortLeg, 602000);
    REQUIRE(memory.RecentlyBlocked(shortLeg, 602001));
    REQUIRE_FALSE(memory.RecentlyBlocked(shortLeg, 662000));
}

TEST_CASE("Local food grounding yields within eight samples before a separate navigation permit", "[AIWorld][RecoveryNavigation]")
{
    using Status = LivingSurfaceCorridor::Status;
    ActionPosition here{0,0,0,0.5f}, target{0,16,0,0.5f};
    LivingPlanningContext planning;
    planning.Begin(1000, here, here, {}, 1, 1);
    planning.ForageAttempts = 3;
    unsigned turns = 0, edges = 0, totalHeights = 0;
    float previousEdgeEnd = 0;
    while (planning.ForageGround.State == Status::Pending)
    {
        REQUIRE(++turns <= 5);
        uint64 now = 1000 + turns * 100;
        unsigned heightsThisTurn = 0, segmentsThisTurn = 0;
        PlanningWorkBudget budget(std::chrono::seconds(1), 1);
        PlanningWorkBudget::Scope scope(budget);
        {
            auto work = PlanningWorkBudget::TryAcquire();
            REQUIRE(bool(work));
            auto height = [&](ActionPosition const& p) -> std::optional<float>
            {
                ++heightsThisTurn; ++totalHeights;
                float floor = p.X * 0.5f;
                float probe = p.Z - 0.5f + (p.X == 0 ? 0.3f : 0.8f);
                return probe >= floor && probe-floor <= 3 ? std::optional(floor + 0.5f) : std::nullopt;
            };
            auto clear = [&](ActionPosition const& a, ActionPosition const& b)
            {
                ++segmentsThisTurn; ++edges;
                CHECK(a.X == previousEdgeEnd); // No restarted/repeated slope samples after yielding.
                previousEdgeEnd = b.X;
                return true;
            };
            planning.ForageGround.Advance(here, target, height, clear);
        } // Billing must finish before attempting the navmesh operation.
        REQUIRE(heightsThisTurn <= 8);
        REQUIRE(segmentsThisTurn <= 8);
        auto navigation = PlanningWorkBudget::TryAcquire();
        REQUIRE_FALSE(bool(navigation));
        REQUIRE_FALSE(planning.ForageStep.has_value());
        planning.MarkProgress(now);
        planning.MarkDeferred(now, "WORK_BUDGET", "FORAGE");
        REQUIRE(planning.Resume(now + 100, here, here, {}, 1, 1));
        REQUIRE(planning.ForageAttempts == 3);
    }
    REQUIRE(turns == 5);
    REQUIRE(edges == 32);
    REQUIRE(totalHeights == 34); // One endpoint probe + origin +32 samples across five permits.
    REQUIRE(planning.ForageGround.Resolved.has_value());
    REQUIRE(planning.ForageGround.Resolved->Z == 8.5f);
    {
        PlanningWorkBudget budget(std::chrono::seconds(1), 1);
        PlanningWorkBudget::Scope scope(budget);
        auto navigation = PlanningWorkBudget::TryAcquire();
        REQUIRE(bool(navigation));
        planning.ForageStep = planning.ForageGround.Resolved; // Stand-in for the separately admitted Toward result.
        navigation.Finish();
        auto execution = PlanningWorkBudget::TryAcquire();
        REQUIRE_FALSE(bool(execution));
        REQUIRE(planning.ForageStep->Z == 8.5f); // Keep it instead of repeating endpoint/navmesh work.
    }
    auto moved = here; moved.X += 1;
    REQUIRE_FALSE(planning.Resume(1700, moved, here, {}, 1, 1));
    REQUIRE_FALSE(planning.Resume(1700, here, here, {}, 2, 1));
    REQUIRE_FALSE(planning.Resume(1700, here, here, {}, 1, 2));
    planning.Begin(1700, moved, here, {}, 1, 1);
    REQUIRE_FALSE(planning.ForageGround.Started);
    REQUIRE_FALSE(planning.ForageStep.has_value());
    REQUIRE_FALSE(planning.ForageAttemptStarted);
}

TEST_CASE("Advice local ground cursor is discarded with its seed or physical context", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceSearch search;
    ActionPosition here{0,0,0,0}, target{0,16,0,0};
    search.Active = true; search.Lifetime = 1; search.Origin = search.Home = here;
    search.ProgressAt = 1000; search.PhaseMask = search.Capabilities = 1;
    search.Radius = 80; search.ArrivalRadius = 14; search.Clearance = 8;
    auto height = [](ActionPosition const& p) -> std::optional<float>
    { return p.X * 0.5f <= p.Z + 0.8f ? std::optional(p.X * 0.5f) : std::nullopt; };
    REQUIRE(search.LocalGround.Advance(here, target, height, [](auto const&, auto const&) { return true; }) ==
        LivingSurfaceCorridor::Status::Pending);
    REQUIRE(search.Next == 0);
    REQUIRE(search.LocalGround.Surface.NextSample == 7);
    REQUIRE(search.Matches(1100, 1, here, here, {}, false, 1, 1, 80, 14, 8));
    auto moved = here; moved.X = 1;
    REQUIRE_FALSE(search.Matches(1100, 1, moved, here, {}, false, 1, 1, 80, 14, 8));
    REQUIRE_FALSE(search.Matches(1100, 2, here, here, {}, false, 1, 1, 80, 14, 8));
    REQUIRE_FALSE(search.Matches(1100, 1, here, here, {}, false, 2, 1, 80, 14, 8));
    search.NextSeed(1100);
    REQUIRE(search.Next == 1);
    REQUIRE_FALSE(search.LocalGround.Started);
    REQUIRE_FALSE(search.GroundedTarget.has_value());
    REQUIRE_FALSE(search.ResolvedTarget.has_value());
}

TEST_CASE("All-NPC advice admission bounds work and gives waiting agents a turn", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(1, 1000));
    REQUIRE_FALSE(budget.Acquire(2, 1000));
    REQUIRE_FALSE(budget.Acquire(3, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 3000)); // hot caller cannot jump the queue
    REQUIRE(budget.Acquire(2, 3000));
    REQUIRE_FALSE(budget.Acquire(3, 3000));
    REQUIRE(budget.Acquire(3, 5000));
    REQUIRE_FALSE(budget.Acquire(4, 5000));
    REQUIRE(budget.Acquire(4, 40001)); // absent head expires
}

TEST_CASE("A crowd of eligible NPCs shares the global advice budget", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    std::vector<uint64> admitted;
    for (uint64 now = 1000; now <= 255000; now += 1000)
        for (uint64 id = 1; id <= 128; ++id)
            if (std::find(admitted.begin(), admitted.end(), id) == admitted.end() && budget.Acquire(id, now))
                admitted.push_back(id);
    REQUIRE(admitted.size() == 128);
    for (size_t i = 0; i < admitted.size(); ++i) REQUIRE(admitted[i] == i + 1);
}

TEST_CASE("Role movement can follow long detours but stalls and loops terminate", "[AIWorld][RecoveryAdvice]")
{
    LivingMovementWatchdog move;
    move.Begin(1000, {0,0,0,0});
    for (uint64 t = 5000; t <= 120000; t += 5000)
        REQUIRE(move.Continue(1000+t, {0,float(t)/1000,0,0}, false));
    REQUIRE_FALSE(move.Continue(136000, {0,120,0,0}, false));
    REQUIRE(std::string(move.End) == "NO_PROGRESS");
    move.Begin(1000, {0,0,0,0});
    for (uint64 t = 5000; t < 180000; t += 5000)
        REQUIRE(move.Continue(1000+t, {0,float(t % 10000),0,0}, false));
    REQUIRE_FALSE(move.Continue(181000, {0,10,0,0}, false));
    REQUIRE(std::string(move.End) == "DURATION_LIMIT");
    move.Begin(1000, {0,0,0,0});
    REQUIRE_FALSE(move.Continue(9000, {0,10,0,0}, true));
    REQUIRE(std::string(move.End) == "ESCAPE_LIMIT");
}

TEST_CASE("Advice tickets survive long decision sleeps without blocking moving actors", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(1, 1000));
    REQUIRE_FALSE(budget.Acquire(2, 1000));
    REQUIRE_FALSE(budget.Acquire(3, 1000));
    for (uint64 now = 2000; now <= 36000; now += 1000)
    {
        budget.Refresh(2, now, true, false, false); // still moving; keep ticket
        budget.Refresh(3, now, true, false);
        if (now == 3000) REQUIRE(budget.Acquire(3, now));
    }
    REQUIRE(budget.WaitMs(2, 36000) == 35000);
    budget.Refresh(2, 36000, true, false);
    REQUIRE(budget.Ready(2, 36000));
    REQUIRE(budget.Acquire(2, 36000)); // original ticket, not a new tail entry
    REQUIRE_FALSE(budget.WaitMs(2, 36000));
}

TEST_CASE("Slow decision makers receive advice in a sustained four hour crowd", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    std::array<uint64, 121> next{}, requests{};
    for (uint64 now = 1000; now <= 14400000; now += 1000)
    {
        for (uint64 id = 0; id < next.size(); ++id)
        {
            bool eligible = now >= next[id];
            budget.Refresh(id, now, eligible, id == 120);
            // Runtime wakes an idle/resting actor at its turn. Otherwise this
            // actor normally makes a local decision only every 35 seconds.
            if (!eligible || (now % (id == 120 ? 35000 : 5000) && !budget.Ready(id, now))) continue;
            if (budget.Acquire(id, now, id == 120))
            { ++requests[id]; next[id] = now + 120000; }
        }
    }
    for (auto count : requests) REQUIRE(count >= 20);
}

TEST_CASE("Urgent returns get priority while food requests and cancellation remain live", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceBudget budget;
    REQUIRE(budget.Acquire(99, 1000));
    REQUIRE_FALSE(budget.Acquire(1, 1000));
    for (uint64 id = 2; id <= 5; ++id) REQUIRE_FALSE(budget.Acquire(id, 1000, true));
    REQUIRE(budget.Acquire(2, 3000, true));
    REQUIRE(budget.Acquire(3, 5000, true));
    REQUIRE(budget.Acquire(4, 7000, true));
    REQUIRE_FALSE(budget.Acquire(5, 9000, true));
    REQUIRE(budget.Acquire(1, 9000));
    budget.Refresh(5, 10000, false, true); // dead, combat, changed goal or home
    REQUIRE_FALSE(budget.WaitMs(5, 10000));
    REQUIRE(budget.Size() == 0);
}

TEST_CASE("Escape progress survives generator restarts and reacts to a changed threat", "[AIWorld][RecoveryAdvice]")
{
    LivingEscapeProgress escape;
    ActionPosition here{0,10,0,0}, danger{0,0,0,0};
    REQUIRE_FALSE(escape.Observe(1000, here, danger));
    REQUIRE_FALSE(escape.Observe(6000, here, danger));
    REQUIRE(escape.Observe(9000, here, danger));
    escape.Routes.Reject(here, {0,26,0,0});
    escape.Failed(9000);
    REQUIRE(escape.RetryAt == 11000);
    REQUIRE(escape.Observe(40000, here, danger)); // restarting did not erase it
    REQUIRE(escape.Routes.Failed(here, {0,26,0,0}));
    REQUIRE_FALSE(escape.Routes.Failed(here, {0,18,8,0}));
    REQUIRE_FALSE(escape.Observe(41000, here, {0,3,0,0}));
    REQUIRE(escape.Failures == 0);
    REQUIRE_FALSE(escape.Observe(47000, {0,12,0,0}, danger));
}

TEST_CASE("Ground recovery needs agreeing support and a collision free settling leg", "[AIWorld][RecoveryAdvice]")
{
    using namespace LivingReturnPolicy;
    auto flat = [](auto const&) -> std::optional<float> { return 0.0f; };
    auto clear = [](auto const&, auto const&) { return true; };
    ActionPosition from{0,0,0,2.2f}, to{0,4,0,0}, support{0,0,0,0.1f};
    char const* reason = "NONE";
    REQUIRE(SurfaceConnector(from, to, flat, clear).empty());
    auto path = SurfaceConnector(from, to, flat, clear, &reason, support);
    REQUIRE(path.size() == 10);
    REQUIRE(path.front().Z == from.Z); // no teleport or hidden start change
    REQUIRE(path[1].Z == 0);
    REQUIRE(path.back().X == 4);
    REQUIRE(SurfaceConnector(from, to, flat, clear, &reason, ActionPosition{0,0,0,2.2f}).empty());
    REQUIRE_FALSE(SurfaceConnector(from, to, flat, clear, &reason, ActionPosition{0,2,0,0}).empty());
    REQUIRE(SurfaceConnector({0,0,0,4}, to, flat, clear, &reason, support).empty());
    REQUIRE(SurfaceConnector(from, to, flat, [](auto const& a, auto const& b) { return a.Z == b.Z; }, &reason, support).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_START_OBSTACLE");
    REQUIRE(SurfaceConnector(from, to, [](auto const& p) -> std::optional<float> { return p.X > 2 ? -5.0f : 0.0f; }, clear, &reason, support).empty());
}

TEST_CASE("Food memory distinguishes a meal from a search and forgets stale evidence", "[AIWorld][RecoveryAdvice]")
{
    LivingFoodMemory memory;
    ActionPosition home{0,0,0,0}, food{0,40,0,0};
    memory.Searched(food, 1000);
    REQUIRE_FALSE(memory.FoodHint(home, 80, 1000).has_value());
    REQUIRE(memory.Visits(food, 1000) == 1);
    memory.EmptyRound(); memory.EmptyRound(); memory.EmptyRound();
    REQUIRE(LivingForagePolicy::SearchRadius(memory.EmptyRounds) == 128);
    for (uint32 leg = 0; leg < 120; ++leg)
    {
        auto waypoint = LivingForagePolicy::Waypoint(home, 80992, leg, memory.EmptyRounds);
        REQUIRE(LivingReturnPolicy::Distance(home, waypoint) <= 128);
    }
    REQUIRE(memory.AdviceFailed() == 240000);
    REQUIRE(memory.AdviceFailed() == 480000);
    REQUIRE(memory.AdviceFailed() == 900000);
    REQUIRE(memory.AdviceFailed() == 900000);
    memory.Fed(food, 2000);
    REQUIRE(memory.FailedAdvice == 0);
    REQUIRE(memory.EmptyRounds == 0);
    REQUIRE(memory.FoodHint(home, 80, 2000).has_value());
    REQUIRE_FALSE(memory.FoodHint({1,0,0,0}, 80, 2000).has_value());
    REQUIRE_FALSE(memory.FoodHint(home, 30, 2000).has_value());
    memory.Searched(food, 3000);
    REQUIRE_FALSE(memory.FoodHint(home, 80, 4000).has_value());
    REQUIRE(memory.FoodHint(home, 80, 603000).has_value());
    REQUIRE_FALSE(memory.FoodHint(home, 80, 1802000).has_value());
    for (unsigned i = 0; i < 100; ++i)
    { memory.Searched({0,float(i)*10,0,0}, 2000000+i); memory.Fed({0,float(i)*10,100,0}, 2000000+i); }
    REQUIRE(memory.Searches.size() == 32);
    REQUIRE(memory.Meals.size() == 8);
}

TEST_CASE("Recovery response cannot invent geometry or alter the response schema", "[AIWorld][RecoveryAdvice]")
{
    RecoveryAdviceResponse response;
    REQUIRE(ParseRecoveryAdvice(R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2})", response));
    REQUIRE(response.Token == 2);
    REQUIRE(response.Agent.Value == 80447);
    for (auto json : {
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2,"x":1})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":true})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2.0})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":2,"choice":0})",
        R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":9})",
        R"({"protocol_version":1,"request_id":18446744073709551616,"agent_id":80447,"episode":123,"choice":2})",
        R"({"protocol_version":2,"request_id":9,"agent_id":80447,"episode":123,"choice":2})"})
        REQUIRE_FALSE(ParseRecoveryAdvice(json, response));
    REQUIRE(ParseRecoveryAdvice(R"({"protocol_version":1,"request_id":9,"agent_id":80447,"episode":123,"choice":0})", response));
    REQUIRE(response.Token == 0);
}

TEST_CASE("Recovery advice is stale after displacement home changes or deadline", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceState state;
    state.PendingId = 9; state.RequestedAt = 1000;
    state.Origin = {0, 10, 10, 10}; state.Home = {0, 0, 0, 0};
    REQUIRE(state.Fresh(31000, state.Origin, state.Home));
    REQUIRE_FALSE(state.Fresh(31001, state.Origin, state.Home));
    REQUIRE_FALSE(state.Fresh(999, state.Origin, state.Home));
    REQUIRE_FALSE(state.Fresh(2000, {0, 13, 10, 10}, state.Home));
    REQUIRE_FALSE(state.Fresh(2000, {1, 10, 10, 10}, state.Home));
    REQUIRE_FALSE(state.Fresh(2000, state.Origin, {0, 1, 0, 0}));
    state.ClearPending();
    REQUIRE_FALSE(state.Fresh(2000, state.Origin, state.Home));
}

TEST_CASE("Recovery choice belongs to the exact request agent episode and offered tokens", "[AIWorld][RecoveryAdvice]")
{
    RecoveryAdviceRequest request;
    request.RequestId = 9; request.Agent = AgentId{80447}; request.Episode = 123;
    request.Options = {{2, "TRAIL"}};
    RecoveryAdviceResponse response{9, 123, AgentId{80447}, 2};
    REQUIRE(MatchesRecoveryAdvice(request, response));
    auto other = response; other.RequestId++;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other = response; other.Agent.Value++;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other = response; other.Episode++;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other = response; other.Token = 1;
    REQUIRE_FALSE(MatchesRecoveryAdvice(request, other));
    other.Token = 0;
    REQUIRE(MatchesRecoveryAdvice(request, other));
}

TEST_CASE("Successful recovery memory is bounded and consumes only the active step", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceState state;
    state.RememberSuccess();
    REQUIRE(state.Successful.empty());
    for (unsigned i = 0; i < 10; ++i)
    {
        state.Active = LivingAdviceCandidate{};
        state.ActiveOrigin = {0, float(i), 0, 0};
        state.RememberSuccess();
        REQUIRE_FALSE(state.Active.has_value());
    }
    REQUIRE(state.Successful.size() == 8);
    REQUIRE(state.Successful.front().From.X == 2);
    REQUIRE(state.Successful.back().From.X == 9);
}

TEST_CASE("Return corridor preserves corners and failed execution edges", "[AIWorld][RecoveryAdvice]")
{
    using namespace LivingReturnPolicy;
    auto plan = Corridor({{0,0,0,0}, {0,0,28,0}, {0,28,28,0}, {0,28,0,0}});
    REQUIRE(plan.size() == 6);
    REQUIRE(plan[1].Y == 28);
    REQUIRE(plan[1].X == 0);
    REQUIRE(plan.back().Y == 0);
    RouteMemory memory;
    memory.Planned = plan;
    memory.Advance({0,0,14,0});
    REQUIRE(memory.Planned.front().Y == 28);
    memory.Reject({0,0,14,0}, {0,0,28,0});
    REQUIRE(memory.Planned.empty());
    REQUIRE(memory.Failed({0,0,15,0}, {0,0,28,0}));
    REQUIRE_FALSE(memory.Failed({0,0,28,0}, {0,0,14,0}));
    REQUIRE_FALSE(memory.Failed({0,5,14,0}, {0,0,28,0}));
    REQUIRE(Corridor({{0,0,0,0}, {0,1000,0,0}}).empty());
    REQUIRE(Corridor({{0,0,0,0}, {1,5,0,0}}).empty());
    REQUIRE(HomeLimit(30) == 96);
    REQUIRE(HomeLimit(80) == 144);
}

TEST_CASE("Return rejection distinguishes a wall from unsupported terrain", "[AIWorld][RecoveryAdvice]")
{
    char const* reason = nullptr;
    auto clear = [](auto const&, auto const&) { return true; };
    auto flat = [](auto const&) -> std::optional<float> { return 0.0f; };
    using namespace LivingReturnPolicy;
    REQUIRE(SurfaceConnector({0,0,0,0}, {0,4,0,0}, flat, clear, &reason).size() == 9);
    REQUIRE(std::string(reason) == "NONE");
    REQUIRE(SurfaceConnector({0,0,0,0}, {0,4,0,0}, flat, [](auto const&, auto const&) { return false; }, &reason).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_OBSTACLE");
    REQUIRE(SurfaceConnector({0,0,0,2}, {0,4,0,0}, flat, clear, &reason).empty());
    REQUIRE(std::string(reason) == "CONNECTOR_START_HEIGHT");
}

TEST_CASE("Normalized return keeps the exact query endpoint in authorization", "[AIWorld][RecoveryAdvice]")
{
    RecoveryMovement request{{0,1.1f,0,0}, {0,20,0,0}, 96, {}, false};
    request.QueryDestination = {0,2,0,0};
    auto changed = request;
    REQUIRE(request == changed);
    changed.QueryDestination->X += 1;
    REQUIRE_FALSE(request == changed);
    REQUIRE(LivingReturnPolicy::UsefulStep({0,0,0,0}, request.Destination));
    request.Destination = {0,0.2f,0,0};
    REQUIRE_FALSE(LivingReturnPolicy::UsefulStep({0,0,0,0}, request.Destination));
}

TEST_CASE("Recovery navigation tries reachable home area without widening arrival", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    ActionPosition home{0, -9836, -685, 31};
    for (float radius : {2.0f, 6.0f, 14.0f})
    {
        auto targets = HomeTargets(home, radius);
        REQUIRE(targets.size() == 9);
        REQUIRE(Distance(targets.front(), home) == 0);
        for (auto const& target : targets)
        {
            CHECK(target.MapId == home.MapId);
            CHECK(target.Z == home.Z);
            CHECK(std::hypot(target.X-home.X, target.Y-home.Y) < radius);
        }
    }
    CHECK(HomeTargets(home, 0).size() == 1);
    CHECK(HomeTargets(home, -1).empty());
}

TEST_CASE("Recovery navigation does not burn eight rejoins on dead end side steps", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    RouteMemory memory;
    ActionPosition from{0, 50, 0, 0};
    for (unsigned i = 0; i < 8; ++i)
    {
        CHECK_FALSE(memory.PlanRejoin(from, {0, 50, 2.0f + i, 0}, {}));
        CHECK(memory.Rejoins.empty());
        CHECK(memory.Planned.empty());
    }
    ActionPosition join{0, 50, 4, 0};
    auto continuation = Corridor({join, {0, 25, 4, 0}, {0, 0, 0, 0}});
    REQUIRE(memory.PlanRejoin(from, join, continuation));
    memory.CommitRejoin(from, join);
    CHECK(memory.Rejoins.size() == 1);
    REQUIRE_FALSE(memory.Planned.empty());
    CHECK(memory.Planned.back().X == 0);
    CHECK_FALSE(memory.PlanRejoin(from, join, continuation));
    auto first = memory.Planned.front();
    REQUIRE(memory.Advance(first));
    CHECK(memory.Planned.size() == continuation.size() - 1);
}

TEST_CASE("Recovery navigation remembers ineffective reached steps without erasing new corridor", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    RouteMemory memory;
    memory.Planned = {{0, 10, 0, 0}, {0, 0, 0, 0}};
    memory.MarkIneffective({0, 50, 0, 0}, {0, 50, 4, 0});
    CHECK_FALSE(memory.Allows({0, 50, 0, 0}, {0, 50, 4, 0}, false, false));
    CHECK_FALSE(memory.Allows({0, 50, 0, 0}, {0, 50, 4, 0}, false, true));
    CHECK(memory.Planned.size() == 2);
    CHECK(memory.Allows({0, 50, 0, 0}, {0, 40, 0, 0}, true, false));
}

TEST_CASE("Home ring resolves supported slopes beyond the spawn height tolerance", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    ActionPosition home{0, 0, 0, 0};
    auto clear = [](auto const&, auto const&) { return true; };
    for (float slope : {-0.5f, 0.5f})
    {
        auto height = [=](ActionPosition const& p) -> std::optional<float> { return slope*p.X; };
        auto target = GroundHomeTarget(home, {0,13,0,0}, height, clear);
        REQUIRE(target.has_value());
        CHECK(target->Z == Approx(slope*13));
        CHECK(HomeEndpointMatches(home, 14, *target, target));
        CHECK(std::abs(target->Z-home.Z) > 3);
    }
}

TEST_CASE("Home height resolution does not cross a cliff wall hole or stacked floor", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    ActionPosition home{0,0,0,0}, target{0,13,0,0};
    auto clear = [](auto const&, auto const&) { return true; };
    auto slope = [](ActionPosition const& p) -> std::optional<float> { return p.X*0.5f; };
    CHECK_FALSE(GroundHomeTarget(home, target, slope, [](auto const& a, auto const& b)
        { return !(a.X < 6 && b.X >= 6); }).has_value());
    CHECK_FALSE(GroundHomeTarget(home, target, [](ActionPosition const& p) -> std::optional<float>
        { return p.X < 6 ? 0.0f : 8.0f; }, clear).has_value());
    CHECK_FALSE(GroundHomeTarget(home, target, [](ActionPosition const& p) -> std::optional<float>
        { if (p.X >= 6 && p.X <= 7) return std::nullopt; return p.X*0.5f; }, clear).has_value());
    CHECK_FALSE(GroundHomeTarget(home, target, [](auto const&) -> std::optional<float>
        { return std::numeric_limits<float>::quiet_NaN(); }, clear).has_value());
    CHECK_FALSE(GroundHomeTarget(home, {1,13,0,0}, slope, clear).has_value());
    CHECK_FALSE(GroundHomeTarget(home, {0,1000,0,0}, slope, clear).has_value());
}

TEST_CASE("Complete home paths may end anywhere supported inside the existing arrival area", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    ActionPosition home{0,0,0,0}, end{0,9,0,4.5f};
    auto support = GroundHomeTarget(home, end,
        [](ActionPosition const& p) -> std::optional<float> { return p.X*0.5f; },
        [](auto const&, auto const&) { return true; });
    CHECK(HomeEndpointMatches(home, 14, end, support));
    CHECK_FALSE(HomeEndpointMatches(home, 8, end, support));
    CHECK_FALSE(HomeEndpointMatches(home, 14, {0,9,0,12}, support));
    CHECK_FALSE(HomeEndpointMatches(home, 14, end, std::nullopt));
    CHECK_FALSE(HomeEndpointMatches(home, -1, end, support));
    CHECK_FALSE(HomeEndpointMatches(home, 14, {1,9,0,4.5f}, support));
}

TEST_CASE("Home diagnostics retain complete-path failures across later height rejections", "[AIWorld][RecoveryNavigation]")
{
    using namespace LivingReturnPolicy;
    Diagnostics diagnostics;
    diagnostics.HomePath.Reject(HomePathFailure::NoPath, 8);
    for (unsigned i = 0; i < 7; ++i) diagnostics.HomePath.Reject(HomePathFailure::Ground, 0);
    CHECK(diagnostics.HomePath.Failure == "NO_COMPLETE_PATH");
    CHECK(diagnostics.HomePath.PathType == 8);
    CHECK(diagnostics.HomePath.Rejected[0] == 7);
    CHECK(diagnostics.HomePath.Rejected[1] == 1);
    diagnostics.ContinuationPath.Reject(HomePathFailure::Endpoint, 1);
    CHECK(diagnostics.HomePath.Failure == "NO_COMPLETE_PATH");
    CHECK(diagnostics.ContinuationPath.Failure == "ENDPOINT_MISMATCH");
}
