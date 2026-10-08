/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingPlanningState.h"
#include "GroundedPathSupport.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <string>

namespace
{
    using ReturnStage = LivingReturnSearch::Stage;
    ActionPosition const PipelineOrigin{0, 0, 0, 0};
    ActionPosition const PipelineHome{0, 15, 0, 0};
    auto pipelineFloor = [](ActionPosition const&) -> std::optional<float> { return 0.0f; };
    auto pipelineClear = [](ActionPosition const&, ActionPosition const&) { return true; };
    auto pipelineBounds = [](ActionPosition const& p)
    { return p.X >= -30 && p.X <= 25 && std::abs(p.Y) <= 30; };
    PlanningWorkBudget::TimePoint PipelineTime(uint64 microseconds)
    { return PlanningWorkBudget::TimePoint(std::chrono::microseconds(microseconds)); }

    // Only the engine query adapter is synthetic. Admission, actor/global
    // budgets, stage orchestration and incremental geometry are production code.
    struct PipelineQueries
    {
        uint64 Now;
        unsigned Started = 0;
        bool Deferred = false;

        template<class Query> bool Run(Query&& query)
        {
            auto begin = Now*1000 + Started*100;
            auto permit = PlanningWorkBudget::TryAcquire(PipelineTime(begin));
            if (!permit) { Deferred = true; return false; }
            query();
            permit.Finish(PipelineTime(begin+100));
            ++Started;
            return true;
        }
    };

    bool SupportedLeg(ActionPosition const& from, ActionPosition const& to)
    {
        LivingSurfaceCorridor::Search execution;
        return LivingSurfaceCorridor::Advance(execution, from, to, pipelineFloor,
            pipelineClear, pipelineBounds, 12) == LivingSurfaceCorridor::Status::Complete;
    }

    bool ClearPipelineWall(ActionPosition const& a, ActionPosition const& b)
    {
        return (a.X <= 0.8f && b.X <= 0.8f) || (a.X >= 1.2f && b.X >= 1.2f) ||
            (std::abs(a.Y) >= 1.2f && std::abs(b.Y) >= 1.2f);
    }

    bool GroundedPipelineLeg(ActionPosition const& to, char const** failure)
    {
        struct Point { float x, y, z; };
        std::vector<Point> raw{{0, 0, 0}, {to.X, to.Y, to.Z}}, output;
        return Movement::PrepareGroundedPath(raw, raw.front(), output,
            [](Point const&) -> std::optional<float> { return 0.0f; },
            [](Point const& a, Point const& b)
            { return ClearPipelineWall({0,a.x,a.y,a.z}, {0,b.x,b.y,b.z}); },
            [](Point const& p) { return pipelineBounds({0,p.x,p.y,p.z}); },
            [](Point const&) { return true; }, failure);
    }
}

TEST_CASE("Recovery navigation return pipeline executes a supported short leg before full home or graph work", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    LivingReturnSearch search;
    PlanningWorkScheduler scheduler;
    scheduler.Request(AgentId{1}, 1000); scheduler.BeginFrame(1000, {AgentId{1}});
    PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
    PlanningWorkBudget::Scope scope(budget);
    PlanningWorkBudget::ActorScope actor(scheduler, AgentId{1});
    PipelineQueries queries{1000};
    unsigned homeQueries = 0, graphQueries = 0;
    ActionPosition live = PipelineOrigin;
    auto step = search.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
    {
        if (stage == ReturnStage::Direct)
        {
            ActionPosition candidate{0, 3, 0, 0};
            bool valid = false;
            if (!queries.Run([&] { valid = SupportedLeg(live, candidate); })) return std::nullopt;
            if (valid) return candidate;
        }
        if (stage == ReturnStage::Home) ++homeQueries;
        if (stage == ReturnStage::SurfaceDetour) ++graphQueries;
        return std::nullopt;
    }, [&] { return queries.Deferred; });
    REQUIRE(step.has_value());
    CHECK(homeQueries == 0); CHECK(graphQueries == 0);
    CHECK_FALSE(search.HomeDetourDeadline.Started);
    CHECK(budget.GetStatistics().Started == 1);
    // A selected route has not yet moved the actor, and is not a home arrival.
    CHECK(LivingReturnPolicy::SamePosition(live, PipelineOrigin));
    CHECK(LivingReturnPolicy::Distance(live, PipelineHome) == 15);
    live = *step; // Explicit execution of the checked synthetic-world leg.
    CHECK(LivingReturnPolicy::Distance(live, PipelineOrigin) == 3);
    CHECK(LivingReturnPolicy::Distance(live, PipelineHome) > 3);
}

TEST_CASE("Recovery navigation return retries retain graph progress through sparse permits and stationary care", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    using Status = LivingSurfaceCorridor::Status;
    LivingReturnSearch search;
    LivingPlanningContext planning;
    planning.Begin(1000, PipelineOrigin, PipelineHome, std::nullopt, 1, 1);
    PlanningWorkScheduler scheduler;
    unsigned slices = 0, fallbackRounds = 0, directCalls = 0, graphCalls = 0;
    unsigned previousEdges = 0;
    std::vector<ActionPosition> route;
    std::optional<ActionPosition> result;
    // A loaded population can leave this actor one query per ten seconds.
    // A complete route needs more than a single thirty-second graph slice.
    for (uint64 now = 1000; now <= 5000000 && !result; now += 10000)
    {
        scheduler.Request(AgentId{1}, now); scheduler.BeginFrame(now, {AgentId{1}});
        PlanningWorkBudget budget(std::chrono::microseconds(2000), 1);
        PlanningWorkBudget::Scope scope(budget);
        PlanningWorkBudget::ActorScope actor(scheduler, AgentId{1});
        PipelineQueries queries{now};
        result = search.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
        {
            if (stage == ReturnStage::Direct) ++directCalls;
            if (stage == ReturnStage::Home && !search.Home.HasContext)
            {
                search.Home.Begin(PipelineOrigin, PipelineHome, 3, 96, nullptr, 8);
                search.Home.NextTarget = search.Home.NextSurfaceTarget = 9;
                search.Home.Done = true; search.Home.SurfaceEligible.fill(true);
            }
            if (stage == ReturnStage::SurfaceDetour)
            {
                if (!search.CanContinueHomeDetour(now))
                {
                    search.Home.Report.DetourFailure = "SURFACE_DETOUR_SLICE_LIMIT";
                    return std::nullopt;
                }
                ++graphCalls;
                while (search.Home.Detour.State == Status::Pending)
                {
                    if (!queries.Run([&]
                        {
                            LivingSurfaceDetour::Advance(search.Home.Detour, PipelineOrigin, PipelineHome, 3, 96,
                                pipelineFloor, ClearPipelineWall, pipelineBounds, [](ActionPosition const& p)
                                { return std::hypot(p.X-PipelineHome.X,p.Y-PipelineHome.Y) <= 3 && std::abs(p.Z) <= 1; });
                        })) return std::nullopt;
                }
                REQUIRE(search.Home.Detour.State == Status::Complete);
                route = search.Home.TakeDetourRoute();
                REQUIRE_FALSE(route.empty());
                return route.front();
            }
            if (stage == ReturnStage::Backtrack) ++fallbackRounds;
            return std::nullopt;
        }, [&] { return queries.Deferred; });
        CHECK(budget.GetStatistics().Started <= 1);
        CHECK(search.Home.Detour.EdgeAttempts >= previousEdges);
        previousEdges = search.Home.Detour.EdgeAttempts;
        if (result) break;
        if (queries.Deferred)
        {
            if (queries.Started) planning.MarkProgress(now);
            planning.MarkDeferred(now, "WORK_BUDGET", "RETURN");
            continue;
        }
        REQUIRE(search.Current == ReturnStage::Done);
        REQUIRE(search.HomeDetourDeadline.Expired);
        REQUIRE(search.HasPendingHomeDetour());
        // This is the natural FailedReturn -> ambient recovery -> new decision
        // boundary. Care must not erase the graph before context validation.
        search.InterruptForAction(true);
        planning.ClearWait();
        REQUIRE(search.HasPendingHomeDetour());
        REQUIRE(search.RestartForDecision(planning, now, PipelineOrigin, PipelineHome, std::nullopt, 1, 1, true));
        planning.Begin(now, PipelineOrigin, PipelineHome, std::nullopt, 1, 1);
        REQUIRE_FALSE(search.HomeDetourDeadline.Started);
        REQUIRE(search.Home.Detour.EdgeAttempts == previousEdges);
        ++slices;
    }
    REQUIRE(result.has_value());
    CHECK(slices > 0);
    CHECK(fallbackRounds == slices);
    CHECK(directCalls == slices+1);
    CHECK(graphCalls > slices);
    CHECK(search.Home.Detour.EdgeAttempts > 8);
    CHECK(search.Home.Detour.EdgeAttempts <= LivingSurfaceDetour::MaxEdges);
    CHECK(search.Home.Detour.Nodes.size() <= LivingSurfaceDetour::MaxNodes);
    auto previous = PipelineOrigin;
    for (auto const& endpoint : route)
    {
        LivingSurfaceCorridor::Search execution;
        REQUIRE(LivingSurfaceCorridor::Advance(execution, previous, endpoint, pipelineFloor,
            ClearPipelineWall, pipelineBounds, 12) == Status::Complete);
        previous = endpoint;
    }
    CHECK(std::hypot(previous.X-PipelineHome.X,previous.Y-PipelineHome.Y) <= 3);
}

TEST_CASE("Recovery navigation retained graph invalidates changed requests and interrupted episodes", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    LivingReturnSearch search;
    LivingPlanningContext planning;
    std::optional<ActionPosition> danger = ActionPosition{0, -20, -20, 0};
    planning.Begin(1000, PipelineOrigin, PipelineHome, danger, 1, 1);
    search.Home.Begin(PipelineOrigin, PipelineHome, 3, 96, &*danger, 8);
    search.Home.NextTarget = search.Home.NextSurfaceTarget = 9;
    search.Home.SurfaceEligible.fill(true);
    REQUIRE(search.AllowHomeDetour(1000));
    LivingSurfaceDetour::Advance(search.Home.Detour, PipelineOrigin, PipelineHome, 3, 96,
        pipelineFloor, ClearPipelineWall, pipelineBounds, [](auto const&) { return false; });
    REQUIRE(search.HasPendingHomeDetour());
    auto from = PipelineOrigin, home = PipelineHome;
    uint32 phase = 1, capabilities = 1;
    uint64 now = 32000;
    bool returning = true;
    SECTION("actual movement") { from.X += 0.01f; }
    SECTION("map") { from.MapId = 1; }
    SECTION("home") { home.Z += 0.01f; }
    SECTION("danger") { danger->X += 0.01f; }
    SECTION("danger ended") { danger.reset(); }
    SECTION("phase") { phase = 2; }
    SECTION("capabilities") { capabilities = 2; }
    SECTION("return episode ended") { returning = false; }
    SECTION("clock moved backwards") { now = 999; }
    SECTION("hunting or movement action") { search.InterruptForAction(false); }
    SECTION("new life resets owning return state") { search = {}; }
    SECTION("completed proof is not reused") { search.Home.Detour.State = LivingSurfaceCorridor::Status::Complete; }
    SECTION("exhausted graph is not resumed") { search.Home.Detour.State = LivingSurfaceCorridor::Status::Rejected; }
    REQUIRE_FALSE(search.RestartForDecision(planning, now, from, home, danger, phase, capabilities, returning));
    CHECK_FALSE(search.Home.HasContext);
    CHECK(search.Home.Detour.Nodes.empty());
    CHECK(search.Home.Detour.Route.empty());
    CHECK(search.Current == ReturnStage::Corridor);
}

TEST_CASE("Recovery navigation retained graph cannot reuse changed home radius bounds or clearance", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    LivingReturnSearch search;
    LivingPlanningContext planning;
    planning.Begin(1000, PipelineOrigin, PipelineHome, std::nullopt, 1, 1);
    search.Home.Begin(PipelineOrigin, PipelineHome, 3, 96, nullptr, 8);
    LivingSurfaceDetour::Advance(search.Home.Detour, PipelineOrigin, PipelineHome, 3, 96,
        pipelineFloor, ClearPipelineWall, pipelineBounds, [](auto const&) { return false; });
    REQUIRE(search.RestartForDecision(planning, 32000, PipelineOrigin, PipelineHome, std::nullopt, 1, 1, true));
    float radius = 3, limit = 96, clearance = 8;
    SECTION("arrival band") { radius = 2; }
    SECTION("episode limit") { limit = 90; }
    SECTION("danger clearance") { clearance = 9; }
    // The engine HomeCorridor provider performs this request check before
    // allowing a retained graph to reach SurfaceDetour.
    REQUIRE_FALSE(search.Home.Matches(PipelineOrigin, PipelineHome, radius, limit, nullptr, clearance));
    search.Home.Begin(PipelineOrigin, PipelineHome, radius, limit, nullptr, clearance);
    CHECK_FALSE(search.Home.Detour.Started);
    CHECK(search.Home.Detour.Nodes.empty());
}

TEST_CASE("Recovery navigation return pipeline retains a complete home corridor across an actor budget yield", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    LivingReturnSearch search;
    LivingReturnPolicy::RouteMemory route;
    PlanningWorkScheduler scheduler;
    unsigned homeCalls = 0, directCalls = 0, graphCalls = 0;
    std::optional<ActionPosition> result;
    for (uint64 now = 1000; now <= 2000; now += 1000)
    {
        scheduler.Request(AgentId{1}, now); scheduler.BeginFrame(now, {AgentId{1}});
        PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
        PlanningWorkBudget::Scope scope(budget);
        PlanningWorkBudget::ActorScope actor(scheduler, AgentId{1});
        PipelineQueries queries{now};
        result = search.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
        {
            if (stage == ReturnStage::Corridor && !route.Planned.empty())
            {
                bool valid = false;
                if (!queries.Run([&] { valid = SupportedLeg(PipelineOrigin, route.Planned.front()); }))
                    return std::nullopt;
                if (valid) return route.Planned.front();
            }
            if (stage == ReturnStage::Direct)
            { if (queries.Run([&] { ++directCalls; })) return std::nullopt; }
            if (stage == ReturnStage::Home)
            {
                ++homeCalls;
                // Three separately bounded queries install a complete route.
                for (unsigned n = 0; n < 3; ++n)
                    if (!queries.Run([] {})) return std::nullopt;
                search.Home.Begin(PipelineOrigin, PipelineHome, 3, 96, nullptr, 8);
                search.Home.NextTarget = 9; search.Home.Done = true;
                route.Planned = {{0, -3, 0, 0}, {0, -3, 3, 0}, PipelineHome};
                search.Advance(ReturnStage::Corridor);
            }
            if (stage == ReturnStage::SurfaceDetour) ++graphCalls;
            return std::nullopt;
        }, [&] { return queries.Deferred; });
        CHECK(queries.Started <= 4);
        if (now == 1000)
        {
            REQUIRE_FALSE(result.has_value());
            CHECK(queries.Deferred);
            CHECK(search.Current == ReturnStage::Corridor);
            REQUIRE(route.Planned.size() == 3);
            CHECK(search.Home.NextTarget == 9);
        }
    }
    REQUIRE(result.has_value());
    CHECK(LivingReturnPolicy::SamePosition(*result, route.Planned.front()));
    CHECK(homeCalls == 1); CHECK(directCalls == 1); CHECK(graphCalls == 0);
    CHECK(search.Home.NextTarget == 9);
}

TEST_CASE("Recovery navigation return pipeline bounds a progressing stationary graph and reaches later fallbacks", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    bool care = false;
    SECTION("continuous query work") { }
    SECTION("stationary care counts in the graph wall deadline") { care = true; }
    LivingReturnSearch search;
    LivingPlanningContext planning;
    planning.Begin(1000, PipelineOrigin, PipelineHome, std::nullopt, 1, 1);
    PlanningWorkScheduler scheduler;
    ActionPosition live = PipelineOrigin;
    auto sealedWall = [](ActionPosition const& a, ActionPosition const& b)
    { return (a.X <= 0.8f && b.X <= 0.8f) || (a.X >= 1.2f && b.X >= 1.2f); };
    auto goal = [](ActionPosition const& p)
    { return std::hypot(p.X-PipelineHome.X, p.Y-PipelineHome.Y) <= 3 && std::abs(p.Z) <= 1; };
    unsigned homeCalls = 0, directCalls = 0, graphCalls = 0;
    uint32 largestNodes = 0;
    uint64 fallbackAt = 0, graphStartedAt = 0;
    std::optional<ActionPosition> result;
    for (uint64 now = 1000; now <= 35000 && !result; now += 1000)
    {
        scheduler.Request(AgentId{1}, now); scheduler.BeginFrame(now, {AgentId{1}});
        PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
        PlanningWorkBudget::Scope scope(budget);
        PlanningWorkBudget::ActorScope actor(scheduler, AgentId{1});
        if (care && now == 11000) REQUIRE(planning.PauseForCare(now));
        if (care && now >= 11000 && now < 16000) continue;
        if (care && now == 16000) REQUIRE(planning.ResumeAfterCare(now));
        if (now > 1000)
            REQUIRE(planning.Resume(now, live, PipelineHome, std::nullopt, 1, 1));
        planning.ResumeWork(now);
        PipelineQueries queries{now};
        result = search.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
        {
            if (stage == ReturnStage::Direct)
            { queries.Run([&] { ++directCalls; }); return std::nullopt; }
            if (stage == ReturnStage::Home)
            {
                if (!queries.Run([&] { ++homeCalls; })) return std::nullopt;
                search.Home.Begin(live, PipelineHome, 3, 96, nullptr, 8);
                search.Home.NextTarget = 9; search.Home.NextSurfaceTarget = 9;
                search.Home.SurfaceEligible[0] = true;
                search.Home.GroundTargets[0] = PipelineHome;
                search.Home.Report.Failure = "NO_COMPLETE_PATH";
                return std::nullopt;
            }
            if (stage == ReturnStage::SurfaceDetour)
            {
                if (!search.CanContinueHomeDetour(now)) return std::nullopt;
                if (!graphStartedAt) graphStartedAt = search.HomeDetourDeadline.StartedAt;
                while (search.Home.Detour.State == LivingSurfaceCorridor::Status::Pending)
                {
                    if (!queries.Run([&]
                    {
                        ++graphCalls;
                        LivingSurfaceDetour::Advance(search.Home.Detour, live, PipelineHome, 3, 96,
                            pipelineFloor, sealedWall, pipelineBounds, goal, 8, 1);
                    })) return std::nullopt;
                    CHECK(search.Home.Detour.Route.empty());
                    REQUIRE(search.Home.Detour.State == LivingSurfaceCorridor::Status::Pending);
                    CHECK(search.Home.Detour.Nodes.size() >= largestNodes);
                    largestNodes = uint32(search.Home.Detour.Nodes.size());
                }
            }
            if (stage == ReturnStage::Detour)
            {
                fallbackAt = now;
                ActionPosition candidate{0, -3, 0, 0};
                bool valid = false;
                if (!queries.Run([&] { valid = SupportedLeg(live, candidate) && sealedWall(live, candidate); }))
                    return std::nullopt;
                if (valid) return candidate;
            }
            return std::nullopt;
        }, [&] { return queries.Deferred; });
        CHECK(queries.Started <= 4);
        CHECK(budget.GetStatistics().Started == queries.Started);
        CHECK(budget.GetStatistics().OperationUs <= 400);
        CHECK(LivingReturnPolicy::SamePosition(live, PipelineOrigin));
        if (queries.Started) planning.MarkProgress(now);
        if (!result) planning.MarkDeferred(now, "WORK_BUDGET", "RETURN");
    }
    REQUIRE(result.has_value());
    REQUIRE(graphStartedAt == 1000);
    CHECK(fallbackAt-graphStartedAt == LivingReturnHomeDetourDeadline::WallLimitMs);
    CHECK(search.HomeDetourDeadline.Expired);
    CHECK(homeCalls == 1); CHECK(directCalls == 1);
    CHECK(graphCalls > 40); CHECK(largestNodes > 1);
    CHECK(search.Home.NextTarget == 9); CHECK(search.Home.NextSurfaceTarget == 9);
    CHECK(search.Home.Report.Failure == "NO_COMPLETE_PATH");
    CHECK(search.Home.Detour.State == LivingSurfaceCorridor::Status::Pending);
    CHECK(planning.Resets == 0);
    CHECK_FALSE(search.AllowHomeDetour(fallbackAt+100000)); // No restart for this decision.
    CHECK(LivingReturnPolicy::SamePosition(live, PipelineOrigin));
    live = *result;
    CHECK(LivingReturnPolicy::Distance(live, PipelineOrigin) == 3);
    CHECK_FALSE(goal(live)); // One fallback step is not a completed home return.
}

TEST_CASE("Recovery navigation return pipeline does not reinstall a complete graph proof after execution rejects it", "[AIWorld][RecoveryNavigation][ReturnPipeline]")
{
    bool delayedValidation = false, carePause = false;
    SECTION("execution rejects the proof immediately") { }
    SECTION("budget wait defers execution past the graph deadline") { delayedValidation = true; }
    SECTION("stationary care defers execution past the graph deadline") { delayedValidation = carePause = true; }
    LivingReturnSearch search;
    LivingReturnPolicy::RouteMemory route;
    LivingPlanningContext executionWait;
    PlanningWorkScheduler scheduler;
    ActionPosition home{0, 6, 0, 0};
    auto goal = [&](ActionPosition const& p) { return LivingReturnPolicy::Distance(p, home) <= 3; };
    // Planning saw flat, open terrain. Execution sees a new obstacle on the
    // first forward leg; an independently checked backward step remains safe.
    auto executionClear = [](ActionPosition const& a, ActionPosition const& b) { return b.X <= a.X; };
    unsigned homeQueries = 0, proofDeliveries = 0, rejectedLegs = 0, graphQueries = 0;
    unsigned completedGraphQueries = 0;
    uint64 proofDeliveredAt = 0, validateAfter = 0;
    std::optional<ActionPosition> result;
    for (uint64 now = 1000; now <= 70000 && !result; now += 1000)
    {
        if (validateAfter && now < validateAfter) continue;
        if (validateAfter && !route.Planned.empty())
        {
            if (carePause) REQUIRE(executionWait.ResumeAfterCare(now));
            REQUIRE(executionWait.Resume(now, PipelineOrigin, home, std::nullopt, 1, 1));
            CHECK(executionWait.ProgressAt == proofDeliveredAt);
            CHECK(now-search.HomeDetourDeadline.StartedAt > LivingReturnHomeDetourDeadline::WallLimitMs);
        }
        scheduler.Request(AgentId{1}, now); scheduler.BeginFrame(now, {AgentId{1}});
        PlanningWorkBudget budget(std::chrono::microseconds(2000), 16);
        PlanningWorkBudget::Scope scope(budget);
        PlanningWorkBudget::ActorScope actor(scheduler, AgentId{1});
        PipelineQueries queries{now};
        result = search.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
        {
            if (stage == ReturnStage::Corridor && !route.Planned.empty())
            {
                if (validateAfter && now < validateAfter)
                { queries.Deferred = true; return std::nullopt; }
                LivingSurfaceCorridor::Search execution;
                if (!queries.Run([&]
                {
                    CHECK(LivingSurfaceCorridor::Advance(execution, PipelineOrigin, route.Planned.front(),
                        pipelineFloor, executionClear, pipelineBounds, 12) == LivingSurfaceCorridor::Status::Rejected);
                })) return std::nullopt;
                ++rejectedLegs;
                route.Planned.clear();
            }
            if (stage == ReturnStage::Home && !search.Home.HasContext)
            {
                if (!queries.Run([&] { ++homeQueries; })) return std::nullopt;
                search.Home.Begin(PipelineOrigin, home, 3, 96, nullptr, 8);
                search.Home.NextTarget = search.Home.NextSurfaceTarget = 9;
                search.Home.SurfaceEligible[0] = true; search.Home.GroundTargets[0] = home;
                search.Home.Done = true;
            }
            if (stage == ReturnStage::SurfaceDetour)
            {
                // Only an unfinished graph spends the foreground deadline.
                // A published proof may await leg validation for longer.
                if (!search.CanContinueHomeDetour(now))
                {
                    search.Home.Report.DetourFailure = "SURFACE_DETOUR_SLICE_LIMIT";
                    return std::nullopt;
                }
                while (search.Home.Detour.State == LivingSurfaceCorridor::Status::Pending)
                {
                    if (!queries.Run([&]
                    {
                        ++graphQueries;
                        LivingSurfaceDetour::Advance(search.Home.Detour, PipelineOrigin, home, 3, 96,
                            pipelineFloor, pipelineClear, pipelineBounds, goal, 8, 1);
                    })) return std::nullopt;
                }
                REQUIRE(search.Home.Detour.State == LivingSurfaceCorridor::Status::Complete);
                search.Home.Report.DetourFailure = search.Home.Detour.Failure;
                search.Home.Report.SurfaceCorridor = true;
                search.Home.Report.Failure = "NONE";
                auto proof = search.Home.TakeDetourRoute();
                if (!proof.empty())
                {
                    ++proofDeliveries; completedGraphQueries = graphQueries;
                    REQUIRE(goal(proof.back()));
                    route.Planned = std::move(proof);
                    search.Advance(ReturnStage::Corridor);
                    if (delayedValidation)
                    {
                        proofDeliveredAt = now;
                        validateAfter = now+35000;
                        executionWait.Begin(now, PipelineOrigin, home, std::nullopt, 1, 1);
                        executionWait.MarkDeferred(now, "WORK_BUDGET", "RETURN");
                        if (carePause) REQUIRE(executionWait.PauseForCare(now));
                    }
                }
            }
            if (stage == ReturnStage::Detour)
            {
                ActionPosition fallback{0, -3, 0, 0};
                LivingSurfaceCorridor::Search execution;
                if (!queries.Run([&]
                {
                    REQUIRE(LivingSurfaceCorridor::Advance(execution, PipelineOrigin, fallback,
                        pipelineFloor, executionClear, pipelineBounds, 12) == LivingSurfaceCorridor::Status::Complete);
                })) return std::nullopt;
                return fallback;
            }
            return std::nullopt;
        }, [&] { return queries.Deferred; });
        CHECK(queries.Started <= 4);
    }
    REQUIRE(result.has_value());
    CHECK(proofDeliveries == 1); CHECK(rejectedLegs == 1); CHECK(homeQueries == 1);
    CHECK(graphQueries == completedGraphQueries);
    CHECK(search.Home.Detour.State == LivingSurfaceCorridor::Status::Complete);
    CHECK(std::string(search.Home.Detour.Failure) == "NONE");
    CHECK(std::string(search.Home.Report.DetourFailure) == "NONE");
    CHECK(search.Home.Report.SurfaceCorridor);
    CHECK(std::string(search.Home.Report.Failure) == "NONE");
    CHECK(search.Home.Detour.Route.empty());
    CHECK(search.Home.TakeDetourRoute().empty());
    CHECK_FALSE(search.HomeDetourDeadline.Expired);
    CHECK(LivingReturnPolicy::SamePosition(*result, ActionPosition{0, -3, 0, 0}));
}

TEST_CASE("Recovery navigation return pipeline resumes unvisited home candidates after a grounded execution rejection", "[AIWorld][RecoveryNavigation][ReturnPipeline][HomeContinuation]")
{
    bool useSecondNavmesh = true, firstSurfaceProof = false;
    SECTION("next navmesh candidate provides a physically safe first leg") { }
    SECTION("exhausted navmesh candidates still allow the full physical graph") { useSecondNavmesh = false; }
    SECTION("a rejected issued surface chord does not skip its remaining candidates")
    { useSecondNavmesh = false; firstSurfaceProof = true; }
    LivingReturnSearch search;
    LivingReturnPolicy::RouteMemory route;
    PlanningWorkScheduler scheduler;
    std::vector<std::size_t> navmeshQueries, surfaceIssues;
    unsigned executionRejected = 0, graphQueries = 0;
    std::optional<ActionPosition> result;
    auto targets = LivingReturnPolicy::HomeTargets(PipelineHome, 3);
    REQUIRE(targets.size() == 9);
    auto goal = [](ActionPosition const& p)
    { return std::hypot(p.X-PipelineHome.X,p.Y-PipelineHome.Y) <= 3 && std::abs(p.Z) <= 1; };
    for (uint64 now = 1000; now <= 100000 && !result && search.Current != ReturnStage::Done; now += 1000)
    {
        scheduler.Request(AgentId{1},now); scheduler.BeginFrame(now,{AgentId{1}});
        PlanningWorkBudget budget(std::chrono::microseconds(2000),16);
        PlanningWorkBudget::Scope scope(budget);
        PlanningWorkBudget::ActorScope actor(scheduler,AgentId{1});
        PipelineQueries queries{now};
        auto homeProvider = [&]() -> std::vector<ActionPosition>
        {
            auto& home = search.Home;
            if (!home.HasContext) home.Begin(PipelineOrigin,PipelineHome,3,96,nullptr,8);
            // This is the actual provider's order: a native candidate issues
            // a route before its first leg receives physical execution proof.
            if (home.Done) return {};
            for (; home.NextTarget < targets.size(); ++home.NextTarget)
            {
                auto index = home.NextTarget;
                if (!queries.Run([&]
                {
                    navmeshQueries.push_back(index);
                    home.GroundTargets[index] = targets[index];
                    home.SurfaceEligible[index] = true;
                })) return {};
                if (firstSurfaceProof) continue; // native mesh has a hole
                home.Report.Failure = "NONE";
                ActionPosition first{0,3,0,0};
                if (useSecondNavmesh && index == 1) first = {0,0,3,0};
                home.IssueCorridor(false);
                return {first,targets[index]};
            }
            while (home.NextSurfaceTarget < targets.size())
            {
                auto index = home.NextSurfaceTarget;
                if (!home.Surface.Started) home.Surface.Target = *home.GroundTargets[index];
                if (!queries.Run([&]
                {
                    // Only the first surface proof saw open terrain. The
                    // physical obstacle appears before that proof is executed.
                    LivingSurfaceCorridor::Advance(home.Surface,PipelineOrigin,home.Surface.Target,
                        pipelineFloor,[&](ActionPosition const& a,ActionPosition const& b)
                        { return firstSurfaceProof && index == 0 ? true : ClearPipelineWall(a,b); },
                        pipelineBounds,8);
                })) return {};
                if (home.Surface.State == LivingSurfaceCorridor::Status::Pending) continue;
                if (home.Surface.State == LivingSurfaceCorridor::Status::Complete)
                {
                    auto corridor = LivingSurfaceCorridor::Legs(home.Surface.Route);
                    REQUIRE_FALSE(corridor.empty());
                    surfaceIssues.push_back(index);
                    home.Report.SurfaceCorridor = true;
                    home.IssueCorridor(true);
                    return corridor;
                }
                home.Surface = {};
                ++home.NextSurfaceTarget;
            }
            home.Done = true;
            return {};
        };
        result = search.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
        {
            if (stage == ReturnStage::Corridor && !route.Planned.empty())
            {
                bool safe = false;
                char const* failure = "NONE";
                if (!queries.Run([&] { safe = GroundedPipelineLeg(route.Planned.front(),&failure); })) return std::nullopt;
                if (safe) return route.Planned.front();
                REQUIRE(std::string(failure) == "GROUND_PATH_OBSTACLE");
                ++executionRejected;
                route.Planned.clear();
            }
            if (stage == ReturnStage::Home)
            {
                auto corridor = homeProvider();
                if (!corridor.empty())
                { route.Planned = std::move(corridor); search.Advance(ReturnStage::Corridor); }
            }
            if (stage == ReturnStage::SurfaceDetour)
            {
                // The real SurfaceDetour provider refuses incomplete cheap
                // candidate scans; only an actually exhausted scan may enter.
                if (!search.Home.CandidatesExhausted(targets.size())) return std::nullopt;
                if (!search.CanContinueHomeDetour(now)) return std::nullopt;
                while (search.Home.Detour.State == LivingSurfaceCorridor::Status::Pending)
                {
                    if (!queries.Run([&]
                    {
                        ++graphQueries;
                        LivingSurfaceDetour::Advance(search.Home.Detour,PipelineOrigin,PipelineHome,3,96,
                            pipelineFloor,ClearPipelineWall,pipelineBounds,goal,8,1);
                    })) return std::nullopt;
                }
                auto proof = search.Home.TakeDetourRoute();
                if (!proof.empty())
                { route.Planned = std::move(proof); search.Advance(ReturnStage::Corridor); }
            }
            return std::nullopt;
        },[&] { return queries.Deferred; });
        CHECK(queries.Started <= 4);
    }
    INFO("native queries=" << navmeshQueries.size() << " physical rejects=" << executionRejected << " graph queries=" << graphQueries);
    REQUIRE(result.has_value());
    CHECK(executionRejected >= 1);
    char const* failure = "NONE";
    REQUIRE(GroundedPipelineLeg(*result,&failure));
    if (useSecondNavmesh)
    {
        REQUIRE(navmeshQueries == std::vector<std::size_t>{0,1});
        CHECK(executionRejected == 1);
        CHECK(graphQueries == 0);
    }
    else
    {
        REQUIRE(navmeshQueries == std::vector<std::size_t>{0,1,2,3,4,5,6,7,8});
        CHECK(search.Home.CandidatesExhausted(targets.size()));
        CHECK(graphQueries > 0);
        if (firstSurfaceProof) REQUIRE(surfaceIssues == std::vector<std::size_t>{0});
        else CHECK(executionRejected == 9);
    }
}

TEST_CASE("Recovery navigation home graph exhaustion uses the actual single home target", "[AIWorld][RecoveryNavigation][ReturnPipeline][HomeContinuation]")
{
    LivingReturnSearch search;
    auto targets = LivingReturnPolicy::HomeTargets(PipelineHome,1.0f);
    REQUIRE(targets.size() == 1);
    search.Home.Begin(PipelineOrigin,PipelineHome,1.0f,96,nullptr,8);
    search.Home.IssueCorridor(false);
    // Rejected execution does not retry that same native proof. Its one
    // direct-surface candidate still needs to be tried before entering graph.
    CHECK(search.Home.NextTarget == 1);
    CHECK_FALSE(search.Home.Done);
    CHECK_FALSE(search.Home.CandidatesExhausted(targets.size()));
    search.Home.NextSurfaceTarget = 1; // that physical chord was also rejected
    CHECK(search.Home.CandidatesExhausted(targets.size()));
}
