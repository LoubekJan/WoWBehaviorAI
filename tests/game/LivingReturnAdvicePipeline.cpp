/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingAdviceBudget.h"
#include "Agent/LivingAdviceState.h"
#include "Agent/LivingReturnAdvice.h"
#include "GroundedPathSupport.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <array>

namespace
{
    using Outcome = LivingReturnAdvice::Outcome;
    using ReturnStage = LivingReturnSearch::Stage;
    ActionPosition const AdviceOrigin{0, 0, 0, 0};
    ActionPosition const AdviceHome{0, 0, 80, 0};
    auto adviceFloor = [](ActionPosition const&) -> std::optional<float> { return 0.0f; };
    auto adviceBounds = [](ActionPosition const& p)
    { return p.MapId == 0 && std::hypot(p.X, p.Y-80) <= 96 && p.Z == 0; };
    bool AdviceClear(ActionPosition const& a, ActionPosition const& b)
    {
        // The direct chord hits a wall near home. A checked route around its
        // west end exists, without teleporting or crossing an unsafe segment.
        return (a.Y <= 70 && b.Y <= 70) || (a.Y >= 71 && b.Y >= 71) ||
            (a.X < -35 && b.X < -35) || (a.X > 35 && b.X > 35);
    }
    PlanningWorkBudget::TimePoint AdviceTime(uint64 microseconds)
    { return PlanningWorkBudget::TimePoint(std::chrono::microseconds(microseconds)); }

    struct AdviceQueries
    {
        uint64 Now;
        unsigned Started = 0;
        bool Deferred = false;
        template<class Query> bool Run(Query&& query)
        {
            uint64 begin = Now*1000 + Started*100;
            auto permit = PlanningWorkBudget::TryAcquire(AdviceTime(begin));
            if (!permit) { Deferred = true; return false; }
            query();
            permit.Finish(AdviceTime(begin+100));
            ++Started;
            return true;
        }
    };

    bool AdviceGroundLeg(ActionPosition const& from, ActionPosition const& to, char const** failure = nullptr)
    {
        struct Point { float x, y, z; };
        std::vector<Point> raw{{from.X,from.Y,from.Z},{to.X,to.Y,to.Z}}, output;
        return Movement::PrepareGroundedPath(raw,raw.front(),output,
            [](Point const&) -> std::optional<float> { return 0.0f; },
            [](Point const& a,Point const& b) { return AdviceClear({0,a.x,a.y,a.z},{0,b.x,b.y,b.z}); },
            [](Point const& p) { return adviceBounds({0,p.x,p.y,p.z}); },
            [](Point const&) { return true; },failure);
    }

    // The synthetic adapters stand in for native map/HTTP queries only. The
    // branch order, return stages, resumable geometry, advice state, fair actor
    // admission and leaf budgets are the same production classes as the world.
    struct AdviceActor
    {
        AgentId Id;
        bool Returning;
        bool HasSafeOption;
        LivingReturnSearch Return;
        LivingAdviceState Advice;
        uint64 FirstAcquire = 0;
        unsigned Admissions = 0, NaturalCalls = 0, NaturalQueries = 0, HttpSubmissions = 0;
        unsigned AdviceQueriesDone = 0;

        AdviceActor(uint64 id, bool returning, bool safe) : Id{id}, Returning(returning), HasSafeOption(safe)
        {
            Advice.LifetimeAt = 7;
            Return.Current = ReturnStage::Rejoin;
        }

        std::vector<ActionPosition> ContinueNativeHome(ActionPosition const& from, AdviceQueries& queries)
        {
            auto& home = Return.Continuation;
            if (!home.HasContext) home.Begin(from,AdviceHome,3,96,nullptr,8);
            auto targets = LivingReturnPolicy::HomeTargets(AdviceHome,3);
            for (; home.NextTarget < targets.size(); ++home.NextTarget)
            {
                auto n = home.NextTarget;
                if (!queries.Run([&]
                {
                    // This native mesh cannot prove a complete route. Installed
                    // terrain permits the real incremental surface fallback.
                    home.GroundTargets[n] = targets[n]; home.SurfaceEligible[n] = true;
                    home.Report.Failure = "NO_COMPLETE_PATH";
                })) return {};
            }
            while (home.NextSurfaceTarget < targets.size())
            {
                auto n = home.NextSurfaceTarget;
                if (!home.Surface.Started) home.Surface.Target = *home.GroundTargets[n];
                if (!queries.Run([&]
                {
                    LivingSurfaceCorridor::Advance(home.Surface,from,home.Surface.Target,
                        adviceFloor,AdviceClear,adviceBounds,8);
                })) return {};
                if (home.Surface.State == LivingSurfaceCorridor::Status::Pending) continue;
                if (home.Surface.State == LivingSurfaceCorridor::Status::Complete)
                {
                    auto route = LivingSurfaceCorridor::Legs(home.Surface.Route);
                    home.IssueCorridor(true);
                    return route;
                }
                home.Surface = {}; ++home.NextSurfaceTarget;
            }
            home.Done = true;
            return {};
        }

        Outcome TryNatural(AdviceQueries& queries)
        {
            ++NaturalCalls;
            auto before = queries.Started;
            auto result = Return.Continue([&](ReturnStage stage) -> std::optional<ActionPosition>
            {
                if (stage != ReturnStage::Rejoin) return std::nullopt;
                // Match the production rejoin loop: a checked short connector
                // keeps its home continuation while a leaf query is deferred.
                while (Return.Next < 18)
                {
                    ActionPosition connector{0,-3-float(Return.Next)*0.1f,0,0};
                    if (!Return.ConnectorReady)
                    {
                        bool safe = false;
                        if (!queries.Run([&] { safe = AdviceGroundLeg(AdviceOrigin,connector); })) return std::nullopt;
                        REQUIRE(safe);
                        Return.Connector = connector; Return.ConnectorReady = true;
                    }
                    auto continuation = ContinueNativeHome(*Return.Connector,queries);
                    if (queries.Deferred) return std::nullopt;
                    if (!continuation.empty()) return connector;
                    ++Return.Next; Return.ConnectorReady = false; Return.Connector.reset(); Return.Continuation = {};
                }
                return std::nullopt;
            },[&] { return queries.Deferred; });
            NaturalQueries += queries.Started-before;
            return result ? Outcome::ReturnStep : queries.Deferred ? Outcome::Deferred : Outcome::NoStep;
        }

        Outcome TryAdvice(LivingAdviceBudget& queue, uint64 now, AdviceQueries& queries)
        {
            if (Advice.PendingId) return Outcome::Pending;
            if (now < Advice.CooldownUntil) return Outcome::NoStep;
            auto& search = Advice.Search;
            if (!search.Active)
            {
                if (!queue.Acquire(Id.Value,now,Returning))
                { Advice.Status = "WAITING_TURN"; return Outcome::NoStep; }
                ++Admissions;
                if (!FirstAcquire) FirstAcquire = now;
                search.Active = true; search.Returning = Returning; search.ProgressAt = now;
                search.Lifetime = Advice.LifetimeAt; search.Origin = AdviceOrigin; search.Home = AdviceHome;
                search.Radius = 96; search.ArrivalRadius = 3; search.Clearance = 8;
                search.PhaseMask = 1; search.Capabilities = 1;
                search.Current = Returning ? LivingAdviceSearch::Stage::Routes : LivingAdviceSearch::Stage::Food;
            }
            search.BudgetPause.End(now,search.ProgressAt);
            while (AdviceQueriesDone < 2)
            {
                if (!queries.Run([&]
                {
                    if (AdviceQueriesDone == 0)
                    {
                        LivingAdviceCandidate candidate;
                        candidate.Move = {{0,-3,0,0},AdviceHome,96};
                        candidate.Move.DangerRadius = 8;
                        candidate.ProofArrivalRadius = 3; candidate.ProofPhaseMask = 1; candidate.ProofCapabilities = 1;
                        candidate.HasProofContext = true;
                        REQUIRE(AdviceGroundLeg(AdviceOrigin,candidate.Move.Destination));
                        search.Trial = candidate;
                    }
                    else if (HasSafeOption)
                    {
                        auto& candidate = *search.Trial;
                        auto route = LivingReturnPolicy::Corridor({candidate.Move.Destination,{0,-40,0,0},{0,-40,80,0},AdviceHome});
                        REQUIRE_FALSE(route.empty());
                        auto previous = candidate.Move.Destination;
                        for (auto const& point : route)
                        { REQUIRE(AdviceGroundLeg(previous,point)); previous = point; }
                        REQUIRE(LivingReturnPolicy::HomeEndpointMatches(AdviceHome,3,route.back(),AdviceHome));
                        candidate.Continuation = std::move(route); candidate.Option.Token = 1;
                        REQUIRE(candidate.ProofMatches(AdviceHome,std::nullopt,1,1,96,3,8));
                        search.Candidates.push_back(candidate);
                    }
                    else
                    {
                        // A safe first step alone is insufficient: the full
                        // continuation still hits the physical wall. It must
                        // be rejected locally, without submitting to HTTP.
                        auto previous = search.Trial->Move.Destination;
                        auto route = LivingReturnPolicy::Corridor({previous,AdviceHome});
                        bool supported = true;
                        for (auto const& point : route)
                        {
                            if (!AdviceGroundLeg(previous,point)) { supported = false; break; }
                            previous = point;
                        }
                        REQUIRE_FALSE(supported);
                    }
                    ++AdviceQueriesDone;
                }))
                { search.BudgetPause.Begin(now,search.ProgressAt); Advice.Status = "PLANNING_DEFERRED"; return Outcome::Deferred; }
                search.MarkProgress(now);
            }
            search.Advance(LivingAdviceSearch::Stage::Ready);
            if (search.Candidates.empty())
            {
                Advice.Status = "NO_VALID_OPTIONS"; Advice.CooldownUntil = now+120000; search = {};
                return Outcome::NoStep;
            }
            // The HTTP adapter sees only options with a checked immediate
            // movement and full return continuation. Admission is not HTTP.
            Advice.Candidates = std::move(search.Candidates); search = {};
            Advice.PendingId = 123; Advice.RequestedAt = Advice.Episode = now;
            Advice.Origin = AdviceOrigin; Advice.Home = AdviceHome; Advice.Returning = Returning;
            ++Advice.Requests; ++HttpSubmissions; Advice.Status = "PENDING";
            return Outcome::Pending;
        }
    };

    void QueueActors(LivingAdviceBudget& queue, bool returning)
    {
        REQUIRE(queue.Acquire(99,900,returning)); // Hold the global rate window.
        REQUIRE_FALSE(queue.Acquire(1,1000,returning));
        REQUIRE_FALSE(queue.Acquire(2,1000,returning));
    }
}

TEST_CASE("Recovery advice offered turn reaches admission despite deferred foreground return for ninety seconds", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    LivingAdviceBudget queue;
    QueueActors(queue,true);
    PlanningWorkScheduler scheduler;
    std::array<AdviceActor,2> actors{{{1,true,true},{2,true,false}}};
    for (uint64 now = 4000; now < 94000; now += 1000)
    {
        for (auto& actor : actors) scheduler.Request(actor.Id,now);
        scheduler.BeginFrame(now,{AgentId{1},AgentId{2}});
        PlanningWorkBudget budget(std::chrono::microseconds(2000),16);
        PlanningWorkBudget::Scope scope(budget);
        for (auto& actor : actors)
        {
            PlanningWorkBudget::ActorScope actorScope(scheduler,actor.Id);
            bool eligible = !actor.Advice.PendingId && now >= actor.Advice.CooldownUntil;
            queue.Refresh(actor.Id.Value,now,eligible,true,true);
            bool offered = queue.Ready(actor.Id.Value,now);
            AdviceQueries queries{now};
            LivingReturnAdvice::Select({bool(actor.Advice.PendingId),actor.Advice.Search.Active,offered,true},
                [&] { return actor.TryNatural(queries); }, [&] { return actor.TryAdvice(queue,now,queries); });
            CHECK(queries.Started <= 4);
        }
        CHECK(budget.GetStatistics().Started <= 8);
        CHECK(budget.GetStatistics().OperationUs <= 800);
    }
    INFO("admissions=" << actors[0].Admissions << ',' << actors[1].Admissions);
    REQUIRE(actors[0].Admissions == 1);
    REQUIRE(actors[1].Admissions == 1);
    CHECK(actors[0].FirstAcquire <= 6000); CHECK(actors[1].FirstAcquire <= 8000);
    CHECK(actors[1].FirstAcquire-actors[0].FirstAcquire >= 2000);
    CHECK(actors[0].HttpSubmissions == 1); CHECK(actors[0].Advice.PendingId == 123);
    CHECK(actors[1].HttpSubmissions == 0); CHECK(actors[1].Advice.Status == "NO_VALID_OPTIONS");
    CHECK(actors[1].NaturalQueries > 0);
    CHECK(actors[1].Return.Current == ReturnStage::Rejoin);
    CHECK(actors[1].Return.Continuation.HasContext);
    CHECK(actors[0].Advice.Started == 0); CHECK(actors[0].Advice.HomeSuccess == 0);
    // A submitted request has not yet selected or executed a model movement.
    CHECK(actors[0].Advice.Selected == 0);
}

TEST_CASE("Recovery advice can acquire an offered return turn before a refused geometry leaf", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    LivingAdviceBudget queue;
    QueueActors(queue,true);
    AdviceActor actor{1,true,true};
    PlanningWorkScheduler scheduler;
    scheduler.Request(actor.Id,4000); scheduler.BeginFrame(4000,{actor.Id});
    {
        PlanningWorkBudget exhausted(std::chrono::microseconds(2000),0);
        PlanningWorkBudget::Scope scope(exhausted);
        PlanningWorkBudget::ActorScope actorScope(scheduler,actor.Id);
        AdviceQueries queries{4000};
        auto outcome = LivingReturnAdvice::Select({false,false,queue.Ready(1,4000),true},
            [&] { return actor.TryNatural(queries); }, [&] { return actor.TryAdvice(queue,4000,queries); });
        CHECK(outcome == Outcome::Deferred);
        REQUIRE(actor.Admissions == 1);
        REQUIRE(actor.Advice.Search.Active);
        CHECK(actor.HttpSubmissions == 0);
        CHECK(queries.Started == 0);
        CHECK(exhausted.GetStatistics().Started == 0);
    }
    // This is one admitted search, not a new admission after every budget yield.
    actor.Advice.Search.Next = 5;
    actor.Advice.Search.Continuation.NextTarget = 2;
    auto origin = actor.Advice.Search.Origin;
    scheduler.Request(actor.Id,5000); scheduler.BeginFrame(5000,{actor.Id});
    PlanningWorkBudget available(std::chrono::microseconds(2000),16);
    PlanningWorkBudget::Scope scope(available);
    PlanningWorkBudget::ActorScope actorScope(scheduler,actor.Id);
    AdviceQueries queries{5000};
    auto outcome = LivingReturnAdvice::Select({false,true,false,true},
        [&] { return actor.TryNatural(queries); }, [&] { return actor.TryAdvice(queue,5000,queries); });
    CHECK(outcome == Outcome::Pending);
    CHECK(actor.Admissions == 1); CHECK(actor.HttpSubmissions == 1);
    CHECK(actor.NaturalCalls == 0);
    REQUIRE_FALSE(actor.Advice.Candidates.empty());
    CHECK(LivingReturnPolicy::SamePosition(origin,actor.Advice.Origin));
    CHECK(actor.Advice.Candidates.front().ProofMatches(AdviceHome,std::nullopt,1,1,96,3,8));
}

TEST_CASE("Recovery advice pending or invalid response retains the natural return cursor", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    LivingReturnSearch search;
    search.Current = ReturnStage::Rejoin; search.Next = 4;
    search.ConnectorReady = true; search.Connector = ActionPosition{0,-3,0,0};
    search.Continuation.Begin(*search.Connector,AdviceHome,3,96,nullptr,8);
    search.Continuation.NextTarget = 9; search.Continuation.NextSurfaceTarget = 6;
    LivingAdviceState advice;
    advice.PendingId = 123; advice.Responded = true; advice.Choice = 1;
    LivingAdviceCandidate candidate; candidate.Option.Token = 1; advice.Candidates.push_back(candidate);
    unsigned natural = 0;
    auto deferred = LivingReturnAdvice::Select({true,false,false,true},[&] { ++natural; return Outcome::ReturnStep; },[&]
    {
        REQUIRE_FALSE(bool(advice.FinishChoice(LivingAdviceValidation::Deferred)));
        return Outcome::Deferred;
    });
    REQUIRE(deferred == Outcome::Deferred);
    CHECK(natural == 0); CHECK(advice.PendingId == 123);
    CHECK(search.Next == 4); CHECK(search.Continuation.NextSurfaceTarget == 6);
    auto pending = LivingReturnAdvice::Select({true,false,false,true},[&] { ++natural; return Outcome::ReturnStep; },
        [] { return Outcome::Pending; });
    REQUIRE(pending == Outcome::Pending); CHECK(natural == 0);
    // Explicit care uses the real pause context and does not clear the search.
    LivingPlanningContext planning;
    planning.Begin(4000,AdviceOrigin,AdviceHome,std::nullopt,1,1);
    REQUIRE(planning.PauseForCare(5000)); REQUIRE(planning.ResumeAfterCare(9000));
    CHECK(search.Next == 4); CHECK(search.Continuation.NextSurfaceTarget == 6);
    auto invalid = LivingReturnAdvice::Select({true,false,false,true},[&]
    {
        ++natural;
        REQUIRE(search.Continuation.Matches(*search.Connector,AdviceHome,3,96,nullptr,8));
        ++search.Continuation.NextSurfaceTarget;
        return Outcome::Deferred;
    },[&]
    {
        REQUIRE_FALSE(bool(advice.FinishChoice(LivingAdviceValidation::Invalid)));
        return Outcome::NoStep;
    });
    CHECK(invalid == Outcome::Deferred); CHECK(natural == 1);
    CHECK(advice.PendingId == 0); CHECK(advice.Rejected == 1);
    CHECK(search.Next == 4); CHECK(search.Continuation.NextSurfaceTarget == 7);
}

TEST_CASE("Recovery advice no valid options resumes after a physically rejected issued corridor", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    LivingReturnSearch search;
    search.Current = ReturnStage::Home;
    search.Home.Begin({0,0,69,0},{0,0,80,0},3,96,nullptr,8);
    search.Home.Report.Failure = "NONE";
    search.Home.IssueCorridor(false);
    char const* failure = "NONE";
    REQUIRE_FALSE(AdviceGroundLeg({0,0,69,0},{0,0,72,0},&failure));
    REQUIRE(std::string(failure) == "GROUND_PATH_OBSTACLE");
    LivingAdviceState advice;
    unsigned natural = 0;
    auto outcome = LivingReturnAdvice::Select({false,false,true,true},[&]
    {
        ++natural;
        REQUIRE(search.Home.NextTarget == 1);
        REQUIRE_FALSE(search.Home.Done);
        search.Home.IssueCorridor(false);
        REQUIRE(AdviceGroundLeg({0,0,69,0},{0,-3,69,0}));
        return Outcome::ReturnStep;
    },[&]
    {
        advice.Status = "NO_VALID_OPTIONS"; advice.CooldownUntil = 124000;
        return Outcome::NoStep;
    });
    CHECK(outcome == Outcome::ReturnStep); CHECK(natural == 1);
    CHECK(search.Home.NextTarget == 2); CHECK_FALSE(search.Home.Done);
    CHECK(advice.Status == "NO_VALID_OPTIONS"); CHECK(advice.Requests == 0);
    unsigned attempts = 0;
    outcome = LivingReturnAdvice::Select({false,false,false,false},[] { FAIL("A return retry is not due"); return Outcome::NoStep; },
        [&] { ++attempts; return Outcome::NoStep; });
    CHECK(outcome == Outcome::RetryWait); CHECK(attempts == 1);
    CHECK(search.Home.NextTarget == 2);

    unsigned naturalSteps = 0, unofferedAdvice = 0;
    outcome = LivingReturnAdvice::Select({false,false,false,true},
        [&] { ++naturalSteps; return Outcome::ReturnStep; },
        [&] { ++unofferedAdvice; return Outcome::AdviceStep; });
    CHECK(outcome == Outcome::ReturnStep);
    CHECK(naturalSteps == 1); CHECK(unofferedAdvice == 0);
}

TEST_CASE("Recovery advice offered food turn is admitted before ninety seconds of unavailable hunt leaves", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    LivingAdviceBudget queue;
    QueueActors(queue,false);
    PlanningWorkScheduler scheduler;
    std::array<AdviceActor,2> actors{{{1,false,true},{2,false,false}}};
    for (uint64 now = 4000; now < 94000; now += 1000)
    {
        for (auto& actor : actors) scheduler.Request(actor.Id,now);
        scheduler.BeginFrame(now,{AgentId{1},AgentId{2}});
        PlanningWorkBudget exhausted(std::chrono::microseconds(2000),0);
        PlanningWorkBudget::Scope scope(exhausted);
        for (auto& actor : actors)
        {
            PlanningWorkBudget::ActorScope actorScope(scheduler,actor.Id);
            queue.Refresh(actor.Id.Value,now,true,false,true);
            AdviceQueries queries{now};
            // This is the real early runner used before HUNT/FORAGE and the
            // leaf-unavailable return. Admission itself needs no query permit.
            auto outcome = LivingReturnAdvice::TryPriority(
                {bool(actor.Advice.PendingId),actor.Advice.Search.Active,queue.Ready(actor.Id.Value,now),true},
                [&] { return actor.TryAdvice(queue,now,queries); });
            if (actor.Advice.Search.Active)
            {
                CHECK(outcome == Outcome::Deferred);
                REQUIRE(actor.Advice.Search.Matches(now,7,AdviceOrigin,AdviceHome,std::nullopt,false,1,1,96,3,8));
            }
            CHECK(queries.Started == 0);
        }
        CHECK(exhausted.GetStatistics().Started == 0);
    }
    REQUIRE(actors[0].Admissions == 1); REQUIRE(actors[1].Admissions == 1);
    CHECK(actors[0].FirstAcquire <= 6000); CHECK(actors[1].FirstAcquire <= 8000);
    CHECK(actors[1].FirstAcquire-actors[0].FirstAcquire >= 2000);
    for (auto& actor : actors)
    {
        CHECK(actor.HttpSubmissions == 0);
        CHECK(actor.Advice.Search.Active);
        CHECK(actor.AdviceQueriesDone == 0);
    }
    // A later usable slice resumes those searches; it does not reacquire a
    // ticket or send an empty request just because earlier leaves were denied.
    scheduler.BeginFrame(94000,{AgentId{1},AgentId{2}});
    PlanningWorkBudget available(std::chrono::microseconds(2000),16);
    PlanningWorkBudget::Scope scope(available);
    for (auto& actor : actors)
    {
        PlanningWorkBudget::ActorScope actorScope(scheduler,actor.Id);
        AdviceQueries queries{94000};
        auto outcome = LivingReturnAdvice::TryPriority({false,true,false,true},
            [&] { return actor.TryAdvice(queue,94000,queries); });
        CHECK(outcome == (actor.HasSafeOption ? Outcome::Pending : Outcome::NoStep));
        CHECK(actor.Admissions == 1); CHECK(queries.Started <= 4);
    }
    CHECK(actors[0].HttpSubmissions == 1);
    CHECK(actors[1].HttpSubmissions == 0); CHECK(actors[1].Advice.Status == "NO_VALID_OPTIONS");
}

TEST_CASE("Recovery advice early runner gives no priority without a queued or suspended turn", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    unsigned calls = 0;
    CHECK(LivingReturnAdvice::TryPriority({false,false,false,true},[&] { ++calls; return Outcome::Deferred; }) == Outcome::NoStep);
    CHECK(calls == 0);
    CHECK(LivingReturnAdvice::TryPriority({true,false,false,true},[&] { ++calls; return Outcome::Pending; }) == Outcome::Pending);
    CHECK(calls == 1);
    CHECK(LivingReturnAdvice::TryPriority({false,true,false,true},[&] { ++calls; return Outcome::Deferred; }) == Outcome::Deferred);
    CHECK(calls == 2);
}

TEST_CASE("Recovery advice valid response survives ClearPending and is dispatched once for food and return", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    bool returning = true;
    SECTION("return") { }
    SECTION("food") { returning = false; }
    LivingAdviceState advice;
    advice.PendingId = 123; advice.Responded = true; advice.Choice = 1;
    advice.Requests = 1; advice.Returning = returning;
    LivingAdviceCandidate proof;
    proof.Option.Token = 1; proof.Move = {{0,-3,0,0},AdviceHome,96};
    proof.Move.DangerRadius = 8; proof.ProofArrivalRadius = 3;
    proof.ProofPhaseMask = proof.ProofCapabilities = 1; proof.HasProofContext = true;
    proof.Continuation = LivingReturnPolicy::Corridor({proof.Move.Destination,{0,-40,0,0},{0,-40,80,0},AdviceHome});
    advice.Candidates.push_back(proof);
    std::optional<LivingAdviceCandidate> selected;
    unsigned actualAdviceCalls = 0, naturalCalls = 0, dispatched = 0;
    LivingReturnAdvice::OnceAttempt attempt([&]
    {
        ++actualAdviceCalls;
        REQUIRE(AdviceGroundLeg(AdviceOrigin,advice.ChosenCandidate()->Move.Destination));
        REQUIRE(advice.ChosenCandidate()->ProofMatches(AdviceHome,std::nullopt,1,1,96,3,8));
        selected = advice.FinishChoice(LivingAdviceValidation::Valid);
        return selected ? Outcome::AdviceStep : Outcome::NoStep;
    });
    REQUIRE_FALSE(attempt.Tried());
    auto outcome = LivingReturnAdvice::TryPriority({true,false,false,true},attempt);
    REQUIRE(outcome == Outcome::AdviceStep);
    REQUIRE(selected.has_value());
    CHECK(advice.PendingId == 0); CHECK(advice.Candidates.empty()); CHECK(advice.Selected == 1);
    CHECK(actualAdviceCalls == 1); CHECK(attempt.Tried());
    // ClearPending cleared request storage; the saved movement still carries
    // the checked option and continuation used by the production dispatcher.
    CHECK(selected->Option.Token == 1); CHECK_FALSE(selected->Continuation.empty());
    auto dispatch = [&]
    {
        REQUIRE(AdviceGroundLeg(AdviceOrigin,selected->Move.Destination));
        REQUIRE(selected->ProofMatches(AdviceHome,std::nullopt,1,1,96,3,8));
        ++dispatched;
    };
    if (outcome == Outcome::AdviceStep) dispatch();
    CHECK(dispatched == 1);
    // The shared once wrapper also protects a matching later return fallback
    // from trying to consume the already-cleared pending response a second time.
    outcome = LivingReturnAdvice::Select({false,false,true,true},
        [&] { ++naturalCalls; return Outcome::ReturnStep; },attempt);
    CHECK(outcome == Outcome::AdviceStep);
    CHECK(actualAdviceCalls == 1); CHECK(naturalCalls == 0); CHECK(advice.Selected == 1);
    CHECK(dispatched == 1);
}

TEST_CASE("Recovery advice exhausted options preserve natural hunt and return without double admission", "[AIWorld][RecoveryAdvice][AdvicePipeline]")
{
    bool returning = true;
    SECTION("return continuation") { }
    SECTION("normal hunt fallback") { returning = false; }
    LivingAdviceBudget queue;
    QueueActors(queue,returning);
    AdviceActor actor{1,returning,false};
    PlanningWorkScheduler scheduler;
    scheduler.Request(actor.Id,4000); scheduler.BeginFrame(4000,{actor.Id});
    PlanningWorkBudget budget(std::chrono::microseconds(2000),16);
    PlanningWorkBudget::Scope scope(budget);
    PlanningWorkBudget::ActorScope actorScope(scheduler,actor.Id);
    AdviceQueries queries{4000};
    LivingReturnAdvice::OnceAttempt attempt([&] { return actor.TryAdvice(queue,4000,queries); });
    bool offered = queue.Ready(1,4000);
    REQUIRE(offered);
    auto early = LivingReturnAdvice::TryPriority({false,false,offered,true},attempt);
    REQUIRE(early == Outcome::NoStep);
    REQUIRE(actor.Advice.Status == "NO_VALID_OPTIONS");
    CHECK(actor.Admissions == 1); CHECK(actor.HttpSubmissions == 0);
    auto origin = actor.Advice.Origin;
    unsigned natural = 0;
    LivingPlanningContext planning;
    planning.Begin(4000,AdviceOrigin,AdviceHome,std::nullopt,1,1);
    auto outcome = LivingReturnAdvice::Select({false,false,offered,true},[&]
    {
        ++natural;
        if (returning) return actor.TryNatural(queries);
        // Native nearby-prey/path adapter. The real admitted leaf budget and
        // planning state remain available after NO_VALID_OPTIONS, just as the
        // production early FOOD branch falls through to its unchanged hunt.
        bool safe = false;
        if (!queries.Run([&] { safe = AdviceGroundLeg(AdviceOrigin,{0,-3,0,0}); })) return Outcome::Deferred;
        REQUIRE(safe); planning.HuntScanned = true;
        return Outcome::ReturnStep;
    },attempt);
    CHECK(natural == 1); CHECK(actor.Admissions == 1); CHECK(actor.HttpSubmissions == 0);
    CHECK(actor.Advice.Status == "NO_VALID_OPTIONS");
    CHECK(queries.Started <= 4); CHECK(budget.GetStatistics().Started <= 4);
    if (returning)
    {
        CHECK(outcome == Outcome::Deferred);
        CHECK(actor.Return.Continuation.HasContext);
        CHECK(actor.Return.Continuation.NextTarget > 0);
    }
    else
    { CHECK(outcome == Outcome::ReturnStep); CHECK(planning.HuntScanned); }
    CHECK(LivingReturnPolicy::SamePosition(origin,actor.Advice.Origin));
}
