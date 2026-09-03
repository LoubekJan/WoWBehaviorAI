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

#ifndef AIWORLD_DYNAMICTASKACCEPTANCE_H
#define AIWORLD_DYNAMICTASKACCEPTANCE_H

#include "Define.h"
#include "Goal/GoalType.h"
#include "ObjectGuid.h"
#include "QuestContext.h"
#include "QuestProposalDraft.h"
#include "QuestRequestProvenance.h"

#include <cstddef>
#include <optional>

// Milestone 2.13A3B: what AIWorldMgr remembers about one outstanding
// /dynamic-task request while it is in flight, keyed by AgentId::Value in
// AIWorldMgr::_pendingDynamicTasks. Pure data - Context/Provenance are
// exactly the values SubmitDynamicTask() was called with, captured
// before the async call so a later response is judged against what was
// actually asked, never against anything ai-server claims about itself.
struct PendingDynamicTaskRequest
{
    uint64 RequestId = 0;
    uint64 SubmittedAtMs = 0;

    QuestContext Context;
    QuestRequestProvenance Provenance;
};

// Every way a schema-valid, envelope-matched DynamicTaskResponse can
// still fail world-thread acceptance and be discarded without ever
// producing a DynamicTaskCandidate. StaleTarget is deliberately not
// returned by CheckDynamicTaskResponseAcceptance() below - it requires
// live-re-resolving the actual proposed target Creature*, which stays in
// AIWorldMgr::HandleDynamicTaskResponse() (see that function for why).
enum class DynamicTaskDiscardReason : uint8
{
    None = 0,
    StaleSnapshot,
    StaleRuntime,
    StaleGoal,
    StaleSourceEvent,
    StaleRequest,
    TargetTokenUnbound,
    TargetBindingMismatch,
    PolicyWindowMismatch,
    StaleTarget
};

inline char const* ToString(DynamicTaskDiscardReason reason)
{
    switch (reason)
    {
        case DynamicTaskDiscardReason::StaleSnapshot:        return "STALE_SNAPSHOT";
        case DynamicTaskDiscardReason::StaleRuntime:         return "STALE_RUNTIME";
        case DynamicTaskDiscardReason::StaleGoal:            return "STALE_GOAL";
        case DynamicTaskDiscardReason::StaleSourceEvent:     return "STALE_SOURCE_EVENT";
        case DynamicTaskDiscardReason::StaleRequest:         return "STALE_REQUEST";
        case DynamicTaskDiscardReason::TargetTokenUnbound:   return "TARGET_TOKEN_UNBOUND";
        case DynamicTaskDiscardReason::TargetBindingMismatch:return "TARGET_BINDING_MISMATCH";
        case DynamicTaskDiscardReason::PolicyWindowMismatch: return "REQUEST_POLICY_WINDOW_MISMATCH";
        case DynamicTaskDiscardReason::StaleTarget:          return "STALE_TARGET";
        case DynamicTaskDiscardReason::None:
        default:
            return "NONE";
    }
}

// Milestone 2.13A3B: everything about "does the world thread's current
// state still match what this request was asked about" that does NOT
// require a live Creature/Map/AgentRecord pointer - the caller resolves
// those first (see AIWorldMgr::HandleDynamicTaskResponse()) and passes in
// only the plain values this needs, exactly so CheckDynamicTaskResponseAcceptance()
// (and the staleness/binding rules it encodes) can be exercised by a
// Catch2 test without any live world at all.
struct DynamicTaskAcceptanceState
{
    uint64 CurrentSnapshotSequence = 0;
    ObjectGuid CurrentRuntimeGuid;
    std::optional<GoalType> CurrentGoal;
    uint64 CurrentGoalStartedAtMs = 0;

    // Milestone 2.13A3B review follow-up: SourceEventStillActive alone
    // proved the source memory hadn't been *evicted* yet, but
    // ShortTermMemory's own TTL (AIWorld.ShortTermMemoryTtlMs) is a
    // completely independent value from this request's own
    // AIWorld.DynamicTaskSourceMaxAgeMs - a source event can easily still
    // be "active" well past this request's own staleness policy. The
    // check now also re-derives the source event's age from
    // pending.Provenance.SourceOccurredAtMs (never from the freshly
    // re-found memory's own timestamp - that's still the same event by
    // identity, but this request's own captured provenance stays the
    // single authority for "when did the event this request answers
    // actually occur") against SourceEventMaxAgeMs.
    bool SourceEventStillActive = false;
    uint64 SourceEventMaxAgeMs = 0;

    uint64 NowMs = 0;
    uint64 ResponseMaxAgeMs = 0;
};

// Runs every check below in order, stopping at (and returning) the first
// that fails. Returns None only once every one of them has held -
// callers must still independently live-re-resolve the proposed target
// creature afterward via ResolveDynamicTaskTargetBinding() below (see its
// own comment).
//
// Checks, in order:
//   1. pending.Provenance.SnapshotSequence == state.CurrentSnapshotSequence
//   2. pending.Provenance.RuntimeGuid is non-empty and equals
//      state.CurrentRuntimeGuid - guards against a despawn/respawn
//      incarnation swap under the same AgentId/SpawnId, exactly the
//      protection DecisionProvenance::RuntimeGuid already gives /decision.
//   3. if pending.Provenance.Goal is set (the request was goal-scoped),
//      it (type + StartedAtMs) must match state.CurrentGoal/
//      CurrentGoalStartedAtMs exactly; a request that was NOT
//      goal-scoped is never invalidated by the agent picking up an
//      unrelated goal afterward.
//   4. state.SourceEventStillActive - the WorldEvent memory this request
//      was built from must still be an active short-term memory when the
//      response arrives - AND pending.Provenance.SourceOccurredAtMs
//      (never the freshly re-found memory's own timestamp) is not older
//      than state.SourceEventMaxAgeMs as of state.NowMs. Independent of
//      check 5 below: ShortTermMemory's own TTL and this request's own
//      source-max-age policy are two different values, and a source
//      event can still be "active" well past this request's own policy.
//   5. state.NowMs is not before pending.SubmittedAtMs, and the response
//      did not arrive more than state.ResponseMaxAgeMs after submission.
//   6. draft.TargetToken names a real entry in both
//      pending.Context.CandidateTargets and pending.Provenance.TargetBindings,
//      and those two entries agree on Entry/MapId - the model can never
//      invent a target reference of its own.
//   7. draft.RequiredCount/MaxRangeYards/ExpiryMs/RewardMoneyCopper each
//      fit inside pending.Context.Limits - defense in depth only, never
//      authoritative (see QuestProposalLimits' own comment); 2.13B
//      re-validates independently against current server policy.
DynamicTaskDiscardReason CheckDynamicTaskResponseAcceptance(
    PendingDynamicTaskRequest const& pending,
    QuestProposalDraft const& draft,
    DynamicTaskAcceptanceState const& state);

// Resolves draft.TargetToken to the QuestTargetBinding pending.Provenance
// recorded for it. The caller uses this binding's Guid to live-re-resolve
// the actual proposed target Creature* - callers should only call this
// after CheckDynamicTaskResponseAcceptance() has already returned None
// for this exact (pending, draft) pair, which guarantees a matching
// binding exists.
QuestTargetBinding const* ResolveDynamicTaskTargetBinding(
    PendingDynamicTaskRequest const& pending,
    QuestProposalDraft const& draft);

// True only if a response naming `responseRequestId` for an agent whose
// currently pending request is `pendingRequestId` actually answers that
// pending request. A caller must NEVER erase/consume its pending entry
// for a response this returns false for - an old/foreign response must
// be discarded while leaving a newer in-flight request completely
// untouched (see AIWorldMgr::HandleDynamicTaskResponse()'s own comment on
// why this check runs, and the pending entry is only erased, strictly
// before any other acceptance check).
inline bool DynamicTaskResponseMatchesPending(uint64 pendingRequestId, uint64 responseRequestId)
{
    return pendingRequestId == responseRequestId;
}

// Milestone 2.13A3B review follow-up: the one rule that decides whether
// a /dynamic-task request built from `candidateTargetCount` live-resolved
// candidates is even worth submitting. The only supported objective is
// KILL_CREATURE and every legal draft.target_token must already be one
// of QuestContext::CandidateTargets, so a request with zero candidates
// could never receive a response any target_token in it could legally
// answer - submitting one would just be a guaranteed-hopeless round
// trip. Pure and trivial on purpose: BuildDynamicTaskRequest() needs a
// live Creature/AgentRecord to actually produce candidates, but the rule
// for what to do once it has a count is plain enough to be exercised by
// a Catch2 test without any live world at all.
inline bool CanSubmitDynamicTaskContext(std::size_t candidateTargetCount)
{
    return candidateTargetCount > 0;
}

#endif // AIWORLD_DYNAMICTASKACCEPTANCE_H
