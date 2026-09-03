/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tc_catch2.h"

#include "Inference/DynamicTaskAcceptance.h"

#include <unordered_map>

namespace
{
    QuestTargetCandidate MakeCandidate(uint32 token, uint32 entry, uint32 mapId)
    {
        QuestTargetCandidate candidate;
        candidate.Token = token;
        candidate.Entry = entry;
        candidate.MapId = mapId;
        candidate.DisplayName = "Test Target";
        candidate.DistanceYards = 10.0f;
        candidate.ObservationAgeMs = 100;
        return candidate;
    }

    QuestTargetBinding MakeBinding(uint32 token, uint32 entry, uint32 mapId)
    {
        QuestTargetBinding binding;
        binding.Token = token;
        binding.Entry = entry;
        binding.MapId = mapId;
        binding.Guid = ObjectGuid::Create<HighGuid::Unit>(entry, token);
        return binding;
    }

    // A fully self-consistent pending request + draft pair that
    // CheckDynamicTaskResponseAcceptance() must accept - every negative
    // test below starts from this and breaks exactly one thing.
    PendingDynamicTaskRequest MakeValidPending()
    {
        PendingDynamicTaskRequest pending;
        pending.RequestId = 123;
        pending.SubmittedAtMs = 10000;

        pending.Context.Agent.Value = 42;
        pending.Context.SnapshotSequence = 77;
        pending.Context.CandidateTargets.push_back(MakeCandidate(1, 2002, 0));
        pending.Context.Limits.MaxRequiredCount = 5;
        pending.Context.Limits.MaxRangeYards = 60.0f;
        pending.Context.Limits.MaxExpiryMs = 300000;
        pending.Context.Limits.MaxRewardMoneyCopper = 0;

        pending.Provenance.Agent.Value = 42;
        pending.Provenance.SnapshotSequence = 77;
        pending.Provenance.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        pending.Provenance.SourceEventId = 9001;
        pending.Provenance.SourceCorrelationId = 9002;
        pending.Provenance.SourceEventType = WorldEventType::CreatureKilled;
        pending.Provenance.SourceOccurredAtMs = 9000;
        pending.Provenance.TargetBindings.push_back(MakeBinding(1, 2002, 0));

        return pending;
    }

    QuestProposalDraft MakeValidDraft()
    {
        QuestProposalDraft draft;
        draft.Objective = QuestObjectiveType::KillCreature;
        draft.TargetToken = 1;
        draft.RequiredCount = 3;
        draft.MaxRangeYards = 40.0f;
        draft.ExpiryMs = 200000;
        draft.RewardMoneyCopper = 0;
        draft.Title = "Cull the wolves";
        draft.Description = "Thin the wolf pack near the road.";
        return draft;
    }

    DynamicTaskAcceptanceState MakeMatchingState(PendingDynamicTaskRequest const& pending)
    {
        DynamicTaskAcceptanceState state;
        state.CurrentSnapshotSequence = pending.Provenance.SnapshotSequence;
        state.CurrentRuntimeGuid = pending.Provenance.RuntimeGuid;
        state.CurrentGoal.reset();
        state.CurrentGoalStartedAtMs = 0;
        state.SourceEventStillActive = true;
        state.SourceEventMaxAgeMs = 30000;
        state.NowMs = pending.SubmittedAtMs + 500;
        state.ResponseMaxAgeMs = 10000;
        return state;
    }
}

TEST_CASE("CheckDynamicTaskResponseAcceptance accepts a fully matching response", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::None);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects a stale snapshot", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);
    state.CurrentSnapshotSequence = pending.Provenance.SnapshotSequence + 1;

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleSnapshot);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects a runtime GUID mismatch", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);
    state.CurrentRuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 999);

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleRuntime);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects an empty pending runtime GUID", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    pending.Provenance.RuntimeGuid = ObjectGuid::Empty;
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);
    state.CurrentRuntimeGuid = ObjectGuid::Empty;

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleRuntime);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance goal handling", "[DynamicTaskAcceptance]")
{
    SECTION("goal-scoped request accepts when current goal matches exactly")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        pending.Provenance.Goal = GoalType::FleeDanger;
        pending.Provenance.GoalStartedAtMs = 5000;

        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.CurrentGoal = GoalType::FleeDanger;
        state.CurrentGoalStartedAtMs = 5000;

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::None);
    }

    SECTION("goal-scoped request rejects on goal type mismatch")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        pending.Provenance.Goal = GoalType::FleeDanger;
        pending.Provenance.GoalStartedAtMs = 5000;

        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.CurrentGoal = GoalType::GetFood;
        state.CurrentGoalStartedAtMs = 5000;

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleGoal);
    }

    SECTION("goal-scoped request rejects on StartedAtMs mismatch")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        pending.Provenance.Goal = GoalType::FleeDanger;
        pending.Provenance.GoalStartedAtMs = 5000;

        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.CurrentGoal = GoalType::FleeDanger;
        state.CurrentGoalStartedAtMs = 5001;

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleGoal);
    }

    SECTION("goal-scoped request rejects when the agent no longer has any active goal")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        pending.Provenance.Goal = GoalType::FleeDanger;
        pending.Provenance.GoalStartedAtMs = 5000;

        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.CurrentGoal.reset();

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleGoal);
    }

    SECTION("a request that was NOT goal-scoped is unaffected by the agent later picking up a goal")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        pending.Provenance.Goal.reset();

        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.CurrentGoal = GoalType::GetFood;
        state.CurrentGoalStartedAtMs = 12345;

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::None);
    }
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects when the source event is no longer active", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);
    state.SourceEventStillActive = false;

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleSourceEvent);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance enforces the source event's own max age independently of ShortTermMemory's TTL", "[DynamicTaskAcceptance]")
{
    // Review follow-up: SourceEventStillActive alone only proves
    // ShortTermMemory hasn't evicted the record yet - its TTL
    // (AIWorld.ShortTermMemoryTtlMs) is a completely independent value
    // from AIWorld.DynamicTaskSourceMaxAgeMs. A memory can easily still
    // be "active" well past this request's own source-age policy.
    SECTION("source event age exactly at the max is accepted")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.SourceEventMaxAgeMs = 5000;
        state.NowMs = pending.Provenance.SourceOccurredAtMs + state.SourceEventMaxAgeMs;

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::None);
    }

    SECTION("source event one past the max age is rejected even though the memory is still active")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.SourceEventMaxAgeMs = 5000;
        state.NowMs = pending.Provenance.SourceOccurredAtMs + state.SourceEventMaxAgeMs + 1;
        REQUIRE(state.SourceEventStillActive); // still active - the age check alone must catch this

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleSourceEvent);
    }

    SECTION("a clock reading before the source event occurred is rejected")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.SourceEventMaxAgeMs = 5000;
        state.NowMs = pending.Provenance.SourceOccurredAtMs - 1;

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleSourceEvent);
    }

    SECTION("age is derived from pending.Provenance.SourceOccurredAtMs, never from a differently-timed re-found memory")
    {
        // Even if some other part of the system passed a fresher-looking
        // SourceEventStillActive=true (e.g. the same event re-observed
        // later), this request's own captured provenance timestamp
        // remains the authority for how old ITS source event is.
        PendingDynamicTaskRequest pending = MakeValidPending();
        pending.Provenance.SourceOccurredAtMs = 1000;
        QuestProposalDraft draft = MakeValidDraft();
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);
        state.SourceEventMaxAgeMs = 5000;
        state.NowMs = 6001; // 5001ms after pending's own SourceOccurredAtMs

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleSourceEvent);
    }
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects a request that is too old", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);
    state.NowMs = pending.SubmittedAtMs + state.ResponseMaxAgeMs + 1;

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::StaleRequest);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance accepts a request exactly at the max age boundary", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);
    state.NowMs = pending.SubmittedAtMs + state.ResponseMaxAgeMs;

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::None);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects an absent target token", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();
    draft.TargetToken = 999;
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::TargetTokenUnbound);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects a candidate/binding Entry mismatch", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    pending.Provenance.TargetBindings[0].Entry = 9999; // candidate still says 2002
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::TargetBindingMismatch);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance rejects a candidate/binding MapId mismatch", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    pending.Provenance.TargetBindings[0].MapId = 1; // candidate still says map 0
    QuestProposalDraft draft = MakeValidDraft();
    DynamicTaskAcceptanceState state = MakeMatchingState(pending);

    REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::TargetBindingMismatch);
}

TEST_CASE("CheckDynamicTaskResponseAcceptance enforces the request's own policy window", "[DynamicTaskAcceptance]")
{
    SECTION("required_count over limit")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        draft.RequiredCount = pending.Context.Limits.MaxRequiredCount + 1;
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::PolicyWindowMismatch);
    }

    SECTION("range over limit")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        draft.MaxRangeYards = pending.Context.Limits.MaxRangeYards + 1.0f;
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::PolicyWindowMismatch);
    }

    SECTION("expiry over limit")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        draft.ExpiryMs = pending.Context.Limits.MaxExpiryMs + 1;
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::PolicyWindowMismatch);
    }

    SECTION("reward over limit")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        draft.RewardMoneyCopper = pending.Context.Limits.MaxRewardMoneyCopper + 1;
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::PolicyWindowMismatch);
    }

    SECTION("values exactly at each limit are accepted")
    {
        PendingDynamicTaskRequest pending = MakeValidPending();
        QuestProposalDraft draft = MakeValidDraft();
        draft.RequiredCount = pending.Context.Limits.MaxRequiredCount;
        draft.MaxRangeYards = pending.Context.Limits.MaxRangeYards;
        draft.ExpiryMs = pending.Context.Limits.MaxExpiryMs;
        draft.RewardMoneyCopper = pending.Context.Limits.MaxRewardMoneyCopper;
        DynamicTaskAcceptanceState state = MakeMatchingState(pending);

        REQUIRE(CheckDynamicTaskResponseAcceptance(pending, draft, state) == DynamicTaskDiscardReason::None);
    }
}

TEST_CASE("ResolveDynamicTaskTargetBinding returns the matching binding once accepted", "[DynamicTaskAcceptance]")
{
    PendingDynamicTaskRequest pending = MakeValidPending();
    QuestProposalDraft draft = MakeValidDraft();

    QuestTargetBinding const* binding = ResolveDynamicTaskTargetBinding(pending, draft);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->Token == draft.TargetToken);
    REQUIRE(binding->Entry == 2002);
}

TEST_CASE("DynamicTaskResponseMatchesPending", "[DynamicTaskAcceptance]")
{
    REQUIRE(DynamicTaskResponseMatchesPending(200, 200));
    REQUIRE_FALSE(DynamicTaskResponseMatchesPending(200, 199));
}

TEST_CASE("CanSubmitDynamicTaskContext", "[DynamicTaskAcceptance]")
{
    // The only supported objective is KILL_CREATURE and every legal
    // draft.target_token must already be one of QuestContext::
    // CandidateTargets - a request built with zero candidates could
    // never receive a response any target_token in it could legally
    // answer, so BuildDynamicTaskRequest() must never submit one.
    SECTION("zero candidates - no submission")
    {
        REQUIRE_FALSE(CanSubmitDynamicTaskContext(0));
    }

    SECTION("one or more candidates - submission eligible")
    {
        REQUIRE(CanSubmitDynamicTaskContext(1));
        REQUIRE(CanSubmitDynamicTaskContext(16));
    }
}

TEST_CASE("pending-map regression: an old response never erases a newer pending request", "[DynamicTaskAcceptance]")
{
    // This replicates the exact lookup-then-conditionally-erase algorithm
    // AIWorldMgr::HandleDynamicTaskResponse() runs against
    // _pendingDynamicTasks, using the same DynamicTaskResponseMatchesPending()
    // primitive it does - see that function's own comment for why request-id
    // matching must gate erasure, not just be checked afterward.
    std::unordered_map<uint64, PendingDynamicTaskRequest> pendingByAgent;

    PendingDynamicTaskRequest newer;
    newer.RequestId = 200;
    newer.SubmittedAtMs = 5000;
    pendingByAgent.emplace(42, newer);

    SECTION("a response naming the old request id 199 is discarded, pending 200 remains")
    {
        auto it = pendingByAgent.find(42);
        REQUIRE(it != pendingByAgent.end());

        bool erased = false;
        if (DynamicTaskResponseMatchesPending(it->second.RequestId, 199))
        {
            pendingByAgent.erase(it);
            erased = true;
        }

        REQUIRE_FALSE(erased);
        REQUIRE(pendingByAgent.count(42) == 1);
        REQUIRE(pendingByAgent.at(42).RequestId == 200);
    }

    SECTION("a response naming the current request id 200 is accepted and erases pending")
    {
        auto it = pendingByAgent.find(42);
        REQUIRE(it != pendingByAgent.end());

        bool erased = false;
        if (DynamicTaskResponseMatchesPending(it->second.RequestId, 200))
        {
            pendingByAgent.erase(it);
            erased = true;
        }

        REQUIRE(erased);
        REQUIRE(pendingByAgent.count(42) == 0);
    }
}
