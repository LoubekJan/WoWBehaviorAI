/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingPlanningState.h"
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
                    search.Home.Detour.State = LivingSurfaceCorridor::Status::Rejected;
                    search.Home.Detour.Failure = "SURFACE_DETOUR_TIME_LIMIT";
                    search.Home.Report.DetourFailure = search.Home.Detour.Failure;
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
