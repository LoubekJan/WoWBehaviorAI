/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingAdviceState.h"

namespace
{
    LivingAdviceSearch SearchAt(uint64 now)
    {
        LivingAdviceSearch search;
        search.Active = search.Returning = true;
        search.ProgressAt = now; search.Lifetime = 7;
        search.Origin = {0, 100, 200, 30}; search.Home = {0, 150, 210, 31};
        search.Danger = ActionPosition{0, 90, 200, 30};
        search.PhaseMask = 1; search.Capabilities = 3;
        search.Radius = 200; search.ArrivalRadius = 14; search.Clearance = 8;
        return search;
    }
    bool Matches(LivingAdviceSearch const& search, uint64 now)
    {
        return search.Matches(now, 7, {0, 100, 200, 30}, {0, 150, 210, 31},
            ActionPosition{0, 90, 200, 30}, true, 1, 3, 200, 14, 8);
    }
    LivingAdviceState RespondedAdvice()
    {
        LivingAdviceState advice;
        advice.PendingId = 123; advice.RequestedAt = 1000; advice.Episode = 1000;
        advice.Responded = true; advice.Choice = 2; advice.Requests = 1;
        LivingAdviceCandidate candidate;
        candidate.Option.Token = 2; candidate.Move.Destination = {0, 104, 200, 30};
        candidate.Continuation = {{0, 110, 200, 30}, {0, 150, 210, 31}};
        advice.Candidates.push_back(candidate);
        return advice;
    }
}

TEST_CASE("Planning resume advice expires only after inactivity and invalidates changed geometry context", "[AIWorld][RecoveryAdvice]")
{
    auto search = SearchAt(1000);
    REQUIRE(Matches(search, 30999));
    REQUIRE_FALSE(Matches(search, 31000));
    // A long search remains usable when a later candidate actually advances.
    search.NextSeed(30000);
    REQUIRE(Matches(search, 59999));
    REQUIRE_FALSE(Matches(search, 60000));
    REQUIRE_FALSE(Matches(search, 29999));
    auto original = search;
    search.Origin.X += .01f; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Home.Z += .01f; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Danger->Y += .01f; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Danger.reset(); REQUIRE_FALSE(Matches(search, 30000)); search = original;
    ++search.Lifetime; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    ++search.PhaseMask; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Capabilities = 1; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Radius += 1; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.ArrivalRadius += 1; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Clearance += 1; REQUIRE_FALSE(Matches(search, 30000)); search = original;
    search.Returning = false; REQUIRE_FALSE(Matches(search, 30000));
}

TEST_CASE("Planning resume advice retains proven options while advancing rejected candidates and stages", "[AIWorld][RecoveryAdvice]")
{
    auto search = SearchAt(1000);
    search.Current = LivingAdviceSearch::Stage::Detours;
    search.Seeds.resize(3); search.SeedsReady = true; search.Next = 1;
    search.Candidates.push_back(RespondedAdvice().Candidates.front());
    search.ResolvedTarget = ActionPosition{0, 120, 200, 30};
    search.Trial = LivingAdviceCandidate{};
    search.Continuation.NextTarget = 5;
    search.NextSeed(2000);
    REQUIRE(search.Next == 2);
    REQUIRE(search.ProgressAt == 2000);
    REQUIRE_FALSE(bool(search.ResolvedTarget));
    REQUIRE_FALSE(bool(search.Trial));
    REQUIRE(search.Continuation.NextTarget == 0);
    REQUIRE(search.Candidates.size() == 1);
    search.Probes.NextProbe = 4;
    search.Advance(LivingAdviceSearch::Stage::Ready);
    REQUIRE(search.Next == 0);
    REQUIRE(search.Seeds.empty());
    REQUIRE_FALSE(search.SeedsReady);
    REQUIRE(search.Candidates.size() == 1);
    REQUIRE(search.Probes.NextProbe == 4);
    REQUIRE(search.ProgressAt == 2000); // Changing an empty stage is no terrain progress.
}

TEST_CASE("Planning resume deferred model revalidation keeps the chosen response until a real decision", "[AIWorld][RecoveryAdvice]")
{
    auto advice = RespondedAdvice();
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        REQUIRE_FALSE(bool(advice.FinishChoice(LivingAdviceValidation::Deferred)));
        REQUIRE(advice.PendingId == 123);
        REQUIRE(advice.Responded);
        REQUIRE(advice.Choice == 2u);
        REQUIRE(advice.Candidates.size() == 1);
        REQUIRE(advice.ChosenCandidate()->Continuation.size() == 2);
        REQUIRE(advice.Selected == 0);
        REQUIRE(advice.Rejected == 0);
        REQUIRE(advice.Status == "PLANNING_DEFERRED");
    }
    auto selected = advice.FinishChoice(LivingAdviceValidation::Valid);
    REQUIRE(bool(selected));
    REQUIRE(selected->Option.Token == 2);
    REQUIRE(selected->Continuation.size() == 2);
    REQUIRE(advice.Selected == 1);
    REQUIRE(advice.PendingId == 0);
    REQUIRE(advice.Candidates.empty());
    REQUIRE_FALSE(bool(advice.FinishChoice(LivingAdviceValidation::Valid)));
    REQUIRE(advice.Selected == 1);
}

TEST_CASE("Planning resume distinguishes invalid model geometry from declined or cancelled choices", "[AIWorld][RecoveryAdvice]")
{
    auto advice = RespondedAdvice();
    REQUIRE_FALSE(bool(advice.FinishChoice(LivingAdviceValidation::Invalid)));
    REQUIRE(advice.Rejected == 1);
    REQUIRE(advice.Status == "REVALIDATION_FAILED");
    REQUIRE(advice.PendingId == 0);
    advice = RespondedAdvice(); advice.Choice = 99;
    REQUIRE_FALSE(bool(advice.FinishChoice(LivingAdviceValidation::Invalid)));
    REQUIRE(advice.Rejected == 0);
    REQUIRE(advice.Status == "DECLINED");
    advice = RespondedAdvice(); advice.Search = SearchAt(1000);
    advice.ClearPending();
    REQUIRE_FALSE(advice.Search.Active);
    REQUIRE_FALSE(advice.ChosenCandidate());
    REQUIRE_FALSE(advice.Choice);
}

TEST_CASE("Planning resume cached continuation rejects changed proof context before local revalidation", "[AIWorld][RecoveryAdvice]")
{
    auto candidate = RespondedAdvice().Candidates.front();
    candidate.Move.Home = {0, 150, 210, 31}; candidate.Move.HomeRadius = 200; candidate.Move.DangerRadius = 8;
    candidate.Move.Danger = ActionPosition{0, 90, 200, 30};
    auto matches = [&] { return candidate.ProofMatches({0, 150, 210, 31}, ActionPosition{0, 90, 200, 30}, 1, 3, 200, 14, 8); };
    REQUIRE_FALSE(matches());
    candidate.HasProofContext = true; candidate.ProofPhaseMask = 1; candidate.ProofCapabilities = 3;
    candidate.ProofArrivalRadius = 14;
    REQUIRE(matches());
    auto original = candidate;
    candidate.ProofPhaseMask = 2; REQUIRE_FALSE(matches()); candidate = original;
    candidate.ProofCapabilities = 1; REQUIRE_FALSE(matches()); candidate = original;
    candidate.ProofArrivalRadius = 13; REQUIRE_FALSE(matches()); candidate = original;
    candidate.Move.Danger->X += .01f; REQUIRE_FALSE(matches()); candidate = original;
    candidate.Move.Danger.reset(); REQUIRE_FALSE(matches()); candidate = original;
    candidate.Move.Home.X += .01f; REQUIRE_FALSE(matches()); candidate = original;
    candidate.Move.HomeRadius = 201; REQUIRE_FALSE(matches()); candidate = original;
    candidate.Move.DangerRadius = 9; REQUIRE_FALSE(matches());
}

TEST_CASE("Planning resume advice care pause preserves progress without exempting changed geometry", "[AIWorld][RecoveryAdvice]")
{
    auto search = SearchAt(1000);
    search.Continuation.NextTarget = 6;
    REQUIRE(search.CarePause.Begin(15000, search.ProgressAt));
    REQUIRE_FALSE(Matches(search, 30000)); // No planning while actual care runs.
    REQUIRE(search.CarePause.End(35000, search.ProgressAt));
    REQUIRE(search.ProgressAt == 1000); // Care is not artificial query progress.
    REQUIRE(search.Continuation.NextTarget == 6);
    REQUIRE(Matches(search, 45000)); // 44s total, 20s actual care, 24s inactive.
    REQUIRE_FALSE(Matches(search, 51000));
    search.Home.X += .01f;
    REQUIRE_FALSE(Matches(search, 45000));
    search.Home.X = 150;
    search.NextSeed(45000); // Real work starts a new inactivity epoch.
    REQUIRE(Matches(search, 74999));
    REQUIRE_FALSE(Matches(search, 75000)); // Previous care cannot extend it.
}

TEST_CASE("Planning resume remembered corridor keeps provenance but uses only the current surface marker", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceCandidate candidate;
    candidate.CorridorSeed = true;
    candidate.Move.Destination = {0, 104, 200, 30};
    candidate.Option.Strategy = "KNOWN_SUCCESS"; // Telemetry label must not discard corridor provenance.
    candidate.ConfigureCorridor(true, {{0, 104, 200, 30}}, true);
    REQUIRE(candidate.FollowsCorridor);
    REQUIRE(candidate.Move.SurfaceCorridor);
    candidate.ConfigureCorridor(true, {{0, 104, 200, 30}}, false);
    REQUIRE(candidate.FollowsCorridor);
    REQUIRE_FALSE(candidate.Move.SurfaceCorridor);
    candidate.ConfigureCorridor(true, {{0, 108, 200, 30}}, true);
    REQUIRE_FALSE(candidate.FollowsCorridor);
    REQUIRE_FALSE(candidate.Move.SurfaceCorridor);
    candidate.ConfigureCorridor(false, {{0, 104, 200, 30}}, true);
    REQUIRE_FALSE(candidate.FollowsCorridor);
    REQUIRE_FALSE(candidate.Move.SurfaceCorridor);
    candidate.CorridorSeed = false;
    candidate.ConfigureCorridor(true, {{0, 104, 200, 30}}, true);
    REQUIRE_FALSE(candidate.FollowsCorridor);
    REQUIRE_FALSE(candidate.Move.SurfaceCorridor);
}
