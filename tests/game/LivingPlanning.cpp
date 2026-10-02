/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingAdviceState.h"
#include "Agent/LivingPlanningState.h"
#include <cmath>

namespace
{
    ActionPosition const Origin{0, -9436.498f, -317.804f, 52.99535f};
    ActionPosition const Home{0, -9404.394f, -290.0124f, 62.83232f};
    std::optional<ActionPosition> const Danger{ActionPosition{0, -9410, -300, 60}};
}

TEST_CASE("Planning resume rejects changed geometry phase and movement capability", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    REQUIRE_FALSE(context.Resume(1000, Origin, Home, Danger, 1, 7));
    context.Deferred = true;
    REQUIRE(context.Resume(1000, Origin, Home, Danger, 1, 7));
    auto here = Origin, home = Home;
    auto danger = Danger;
    uint32 phase = 1, capabilities = 7;
    SECTION("one float step of actor movement") { here.X = std::nextafter(here.X, 0.0f); }
    SECTION("actor changes map") { here.MapId = 1; }
    SECTION("home height changes") { home.Z = std::nextafter(home.Z, 0.0f); }
    SECTION("home changes map") { home.MapId = 1; }
    SECTION("remembered danger moves") { danger->Y += 1; }
    SECTION("danger disappears") { danger.reset(); }
    SECTION("phase mask changes") { phase = 2; }
    SECTION("walk or swim capability changes") { capabilities = 5; }
    REQUIRE_FALSE(context.Resume(1200, here, home, danger, phase, capabilities));
}

TEST_CASE("Planning resume rejects a newly remembered danger", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, std::nullopt, 1, 7);
    context.Deferred = true;
    REQUIRE(context.Resume(1200, Origin, Home, std::nullopt, 1, 7));
    REQUIRE_FALSE(context.Resume(1200, Origin, Home, Danger, 1, 7));
}

TEST_CASE("Planning request retains a full corridor after movement but rejects changed route context", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    context.Deferred = true;
    auto moved = Origin;
    moved.X += 5;
    REQUIRE_FALSE(context.Resume(1200, moved, Home, Danger, 1, 7));
    REQUIRE(context.RouteContextMatches(Home, Danger, 1, 7));
    auto home = Home;
    auto danger = Danger;
    uint32 phase = 1, capabilities = 7;
    SECTION("home map changes") { home.MapId = 1; }
    SECTION("home height changes") { home.Z = std::nextafter(home.Z, 0.0f); }
    SECTION("danger moves") { danger->X += 1; }
    SECTION("danger clears") { danger.reset(); }
    SECTION("phase mask changes") { phase = 2; }
    SECTION("anchored capability changes") { capabilities = 3; }
    REQUIRE_FALSE(context.RouteContextMatches(home, danger, phase, capabilities));
}

TEST_CASE("Planning resume keeps long searches with progress and expires idle work", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    context.Deferred = true;
    context.ForageAttempts = 3;
    // A long terrain search can span more than 30 seconds. Actual work keeps
    // its inactivity deadline alive without restarting its original decision.
    for (uint64 now = 1700; now <= 127000; now += 700)
    {
        REQUIRE(context.Resume(now, Origin, Home, Danger, 1, 7));
        context.ProgressAt = now;
        REQUIRE(context.StartedAt == 1000);
        REQUIRE(context.ForageAttempts == 3);
    }
    uint64 lastProgress = context.ProgressAt;
    REQUIRE(context.Resume(lastProgress + 29999, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(lastProgress + 30000, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(999, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(lastProgress - 1, Origin, Home, Danger, 1, 7));
}

TEST_CASE("Planning resume resets age and forage attempts for a new decision", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    context.Deferred = true; context.ProgressAt = 2000; context.ForageAttempts = 6;
    context.Begin(50000, Home, Origin, std::nullopt, 2, 5);
    REQUIRE_FALSE(context.Deferred);
    REQUIRE(context.StartedAt == 50000);
    REQUIRE(context.ProgressAt == 50000);
    REQUIRE(context.ForageAttempts == 0);
    context.Deferred = true;
    REQUIRE(context.Resume(50100, Home, Origin, std::nullopt, 2, 5));
    REQUIRE_FALSE(context.Resume(50100, Origin, Home, Danger, 1, 7));
}

TEST_CASE("Planning resume preserves its resolved connector until the next stage", "[AIWorld][LivingPlanning]")
{
    LivingReturnSearch search;
    ActionPosition raw{0, -9430, -310, 54}, resolved = raw;
    resolved.Z += 0.25f;
    search.Current = LivingReturnSearch::Stage::Rejoin;
    search.Next = 2; search.ConnectorReady = true; search.Connector = resolved;
    search.Continuation.Begin(resolved, Home, 14, 128, nullptr, 8);
    search.Continuation.NextTarget = 4;
    REQUIRE(search.Continuation.Matches(*search.Connector, Home, 14, 128, nullptr, 8));
    REQUIRE_FALSE(search.Continuation.Matches(raw, Home, 14, 128, nullptr, 8));
    // The normalised endpoint remains distinct from the original candidate
    // while a continuation yields. Changing stage must discard that connector.
    REQUIRE(search.Continuation.NextTarget == 4);
    search.Advance(LivingReturnSearch::Stage::Backtrack);
    REQUIRE_FALSE(search.ConnectorReady);
    REQUIRE_FALSE(search.Connector.has_value());
    REQUIRE(search.Next == 0);
}

TEST_CASE("Planning request distinguishes surface corridor permission", "[AIWorld][LivingPlanning]")
{
    RecoveryMovement completeNavmesh{Origin, Home, 128, Danger, false};
    auto completeTerrain = completeNavmesh;
    completeTerrain.SurfaceCorridor = true;
    REQUIRE_FALSE(completeNavmesh == completeTerrain);
    REQUIRE_FALSE(completeTerrain == completeNavmesh);
    auto sameTerrain = completeTerrain;
    REQUIRE(sameTerrain == completeTerrain);
    sameTerrain.SurfaceCorridor = false;
    REQUIRE(sameTerrain == completeNavmesh);
}

TEST_CASE("Planning resume retains a selected advice reply through budget yields", "[AIWorld][LivingPlanning]")
{
    LivingAdviceState advice;
    advice.PendingId = 17; advice.RequestedAt = 1000; advice.Responded = true; advice.Choice = 3;
    advice.Selected = 2; advice.Rejected = 4;
    LivingAdviceCandidate candidate;
    candidate.Option.Token = 3;
    candidate.Move = {Origin, Home, 128, Danger, false};
    candidate.Move.SurfaceCorridor = true;
    candidate.Continuation = {Origin, Home};
    advice.Candidates.push_back(candidate);
    for (unsigned tick = 0; tick < 3; ++tick)
    {
        REQUIRE_FALSE(advice.FinishChoice(LivingAdviceValidation::Deferred).has_value());
        REQUIRE(advice.Status == "PLANNING_DEFERRED");
        REQUIRE(advice.PendingId == 17);
        REQUIRE(advice.RequestedAt == 1000);
        REQUIRE(advice.Responded);
        REQUIRE(advice.Choice == 3u);
        REQUIRE(advice.Candidates.size() == 1);
        REQUIRE(advice.Selected == 2);
        REQUIRE(advice.Rejected == 4);
    }
    SECTION("valid reply selects once and preserves the complete candidate")
    {
        auto selected = advice.FinishChoice(LivingAdviceValidation::Valid);
        REQUIRE(selected.has_value());
        REQUIRE(selected->Move.SurfaceCorridor);
        REQUIRE(selected->Continuation.size() == 2);
        REQUIRE(advice.Selected == 3);
        REQUIRE(advice.Rejected == 4);
        REQUIRE(advice.Status == "SELECTED");
        REQUIRE_FALSE(advice.FinishChoice(LivingAdviceValidation::Valid).has_value());
        REQUIRE(advice.Selected == 3);
        REQUIRE(advice.Rejected == 4);
    }
    SECTION("actual revalidation failure rejects once")
    {
        REQUIRE_FALSE(advice.FinishChoice(LivingAdviceValidation::Invalid).has_value());
        REQUIRE(advice.Status == "REVALIDATION_FAILED");
        REQUIRE(advice.Selected == 2);
        REQUIRE(advice.Rejected == 5);
    }
    REQUIRE(advice.PendingId == 0);
    REQUIRE_FALSE(advice.Responded);
    REQUIRE_FALSE(advice.Choice.has_value());
    REQUIRE(advice.Candidates.empty());
}

TEST_CASE("Planning resume rejects a changed advice search context and idle expiry", "[AIWorld][LivingPlanning]")
{
    LivingAdviceSearch search;
    search.Active = true; search.ProgressAt = 1000; search.Lifetime = 900;
    search.Origin = Origin; search.Home = Home; search.Danger = Danger;
    search.Returning = true; search.PhaseMask = 1; search.Capabilities = 3;
    search.Radius = 128; search.ArrivalRadius = 14; search.Clearance = 8;
    REQUIRE(search.Matches(30999, 900, Origin, Home, Danger, true, 1, 3, 128, 14, 8));
    REQUIRE_FALSE(search.Matches(31000, 900, Origin, Home, Danger, true, 1, 3, 128, 14, 8));
    REQUIRE_FALSE(search.Matches(999, 900, Origin, Home, Danger, true, 1, 3, 128, 14, 8));
    auto here = Origin, home = Home;
    auto danger = Danger;
    uint64 lifetime = 900;
    bool returning = true;
    uint32 phase = 1, capabilities = 3;
    float radius = 128, arrivalRadius = 14, clearance = 8;
    SECTION("different lifetime") { lifetime = 901; }
    SECTION("actor moved") { here.X = std::nextafter(here.X, 0.0f); }
    SECTION("home moved") { home.Z = std::nextafter(home.Z, 0.0f); }
    SECTION("new danger") { danger->X += 1; }
    SECTION("danger cleared") { danger.reset(); }
    SECTION("food instead of return") { returning = false; }
    SECTION("new phase") { phase = 2; }
    SECTION("new movement capability") { capabilities = 1; }
    SECTION("new bounds") { radius = 80; }
    SECTION("new home arrival radius") { arrivalRadius = 15; }
    SECTION("new danger clearance") { clearance = 9; }
    REQUIRE_FALSE(search.Matches(1200, lifetime, here, home, danger, returning, phase, capabilities,
        radius, arrivalRadius, clearance));
}

TEST_CASE("Planning resume advances advice seeds without dropping verified options", "[AIWorld][LivingPlanning]")
{
    LivingAdviceSearch search;
    search.Active = true; search.SeedsReady = true;
    search.Current = LivingAdviceSearch::Stage::Routes;
    search.Seeds.resize(3); search.Next = 1;
    search.ResolvedTarget = Origin; search.Trial.emplace();
    search.Continuation.Begin(Origin, Home, 14, 128, nullptr, 8);
    search.Continuation.NextTarget = 4;
    search.Candidates.emplace_back(); search.Candidates.back().Move.SurfaceCorridor = true;
    search.NextSeed(50000);
    REQUIRE(search.Next == 2);
    REQUIRE(search.ProgressAt == 50000);
    REQUIRE(search.SeedsReady);
    REQUIRE(search.Seeds.size() == 3);
    REQUIRE_FALSE(search.ResolvedTarget.has_value());
    REQUIRE_FALSE(search.Trial.has_value());
    REQUIRE_FALSE(search.Continuation.HasContext);
    REQUIRE(search.Continuation.NextTarget == 0);
    REQUIRE(search.Candidates.size() == 1);
    REQUIRE(search.Candidates.front().Move.SurfaceCorridor);
    search.Advance(LivingAdviceSearch::Stage::Rejoin);
    REQUIRE(search.Next == 0);
    REQUIRE(search.Seeds.empty());
    REQUIRE_FALSE(search.SeedsReady);
    REQUIRE(search.Candidates.size() == 1);
}

TEST_CASE("Planning resume preserves long return work across explicit stationary care", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    context.Deferred = true;
    for (uint64 now = 2000; now <= 59000; now += 1000)
    {
        REQUIRE(context.Resume(now, Origin, Home, Danger, 1, 7));
        context.ProgressAt = now;
    }
    // The caller enters real stationary care instead of erasing its pending
    // return. Pausing must not claim a query happened at the care timestamp.
    context.Deferred = false;
    REQUIRE(context.PauseForCare(60000));
    REQUIRE(context.Deferred);
    REQUIRE(context.CarePause.Active);
    REQUIRE(context.ProgressAt == 59000);
    REQUIRE_FALSE(context.Resume(79999, Origin, Home, Danger, 1, 7));
    REQUIRE(context.ResumeAfterCare(80000));
    REQUIRE_FALSE(context.CarePause.Active);
    REQUIRE(context.ProgressAt == 59000);
    REQUIRE(context.CarePause.CompletedMs == 20000);
    REQUIRE(context.Resume(80000, Origin, Home, Danger, 1, 7));
    REQUIRE(context.Resume(108999, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(109000, Origin, Home, Danger, 1, 7));
    // Real query work after care starts a new idle deadline. Old care is not
    // additional credit for the new progress epoch.
    context.ProgressAt = 80000;
    REQUIRE(context.Resume(109999, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(110000, Origin, Home, Danger, 1, 7));
}

TEST_CASE("Planning resume counts repeated care only until the next actual progress", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    context.Deferred = true;
    REQUIRE(context.PauseForCare(5000));
    REQUIRE(context.ResumeAfterCare(15000));
    REQUIRE(context.PauseForCare(20000));
    REQUIRE(context.ResumeAfterCare(30000));
    REQUIRE(context.CarePause.CompletedMs == 20000);
    REQUIRE(context.ProgressAt == 1000);
    REQUIRE(context.Resume(50999, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(51000, Origin, Home, Danger, 1, 7));
    context.ProgressAt = 30000;
    REQUIRE(context.PauseForCare(35000));
    REQUIRE(context.ResumeAfterCare(40000));
    REQUIRE(context.CarePause.CompletedMs == 5000);
    REQUIRE(context.Resume(64999, Origin, Home, Danger, 1, 7));
    REQUIRE_FALSE(context.Resume(65000, Origin, Home, Danger, 1, 7));
    context.Begin(50000, Origin, Home, Danger, 1, 7);
    REQUIRE_FALSE(context.CarePause.Active);
    REQUIRE(context.CarePause.CompletedMs == 0);
}

TEST_CASE("Planning resume rejects moved actors and backward clocks after care", "[AIWorld][LivingPlanning]")
{
    LivingPlanningContext context;
    context.Begin(1000, Origin, Home, Danger, 1, 7);
    context.Deferred = true;
    REQUIRE(context.PauseForCare(2000));
    SECTION("care endpoint clock moves backwards")
    {
        REQUIRE_FALSE(context.ResumeAfterCare(1999));
        REQUIRE_FALSE(context.Deferred);
        REQUIRE_FALSE(context.CarePause.Active);
    }
    SECTION("a moved actor cannot resume the saved return")
    {
        REQUIRE(context.ResumeAfterCare(22000));
        auto moved = Origin;
        moved.X = std::nextafter(moved.X, 0.0f);
        REQUIRE_FALSE(context.Resume(22000, moved, Home, Danger, 1, 7));
        REQUIRE(context.Resume(22000, Origin, Home, Danger, 1, 7));
        REQUIRE_FALSE(context.Resume(21999, Origin, Home, Danger, 1, 7));
    }
    SECTION("an expired search cannot regain freshness by starting care")
    {
        REQUIRE(context.ResumeAfterCare(22000));
        REQUIRE_FALSE(context.PauseForCare(51000));
        REQUIRE_FALSE(context.CarePause.Active);
    }
}
