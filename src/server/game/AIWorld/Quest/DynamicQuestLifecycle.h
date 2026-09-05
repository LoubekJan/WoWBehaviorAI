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

#ifndef AIWORLD_DYNAMICQUESTLIFECYCLE_H
#define AIWORLD_DYNAMICQUESTLIFECYCLE_H

#include "Define.h"
#include "DynamicQuestInstance.h"
#include "ObjectGuid.h"

#include <optional>

struct QuestProposal;

// Milestone 2.13C1: every way an attempted lifecycle transition can be
// rejected. NotAttempted is the enum's zero value on purpose - a
// default-constructed DynamicQuestTransitionResult must read as
// rejected, never as if a transition had already been judged and passed
// (same pattern as DynamicTaskValidationReason::NotValidated).
enum class DynamicQuestRejectReason : uint8
{
    NotAttempted = 0,
    None,

    // The instance is Completed/Failed/or Expired - no transition is
    // ever legal again, on any function.
    AlreadyTerminal,

    // The instance is in a non-terminal state that does not allow this
    // specific operation (e.g. ApplyDynamicQuestProgress() on an
    // Offered instance, or AcceptDynamicQuest() on an Active one).
    InvalidTransition,

    // OfferDynamicQuest() was called with DynamicQuestId{0} - 0 is that
    // type's own invalid/default value (see DynamicQuestId's own
    // comment), so a genuine lifecycle instance may never carry it.
    InvalidQuestId,

    // Milestone 2.13C2 P2 fix, round 3 (STATIC review): one of
    // DynamicQuestRegistry's own mutation entry points (Accept()/
    // ApplyProgress()/Complete()/Fail()/Expire()) was called with a
    // DynamicQuestId that does not name any currently-stored instance.
    // Never produced by the pure transition functions below themselves -
    // only the registry knows what it currently stores.
    QuestNotFound,

    // Milestone 2.13C2 P3 fix (STATIC review): DynamicQuestRegistry::
    // Offer() minted a new instance whose id already names one it
    // currently stores. Defense in depth only - unreachable given a
    // correct monotonic DynamicQuestId allocator; never produced by
    // OfferDynamicQuest() itself, only by the registry.
    DuplicateQuestId,

    // AcceptDynamicQuest() was called with a playerGuid that is not a
    // real player identity (ObjectGuid::IsPlayer()) - covers both an
    // empty GUID and any other entity type (e.g. a creature GUID).
    InvalidPlayer,

    // The caller's player identity does not match
    // DynamicQuestInstance::AcceptedByPlayerGuid.
    PlayerMismatch,

    // now >= DynamicQuestInstance::ExpiresAtMs, even though State has not
    // been explicitly transitioned to Expired yet - see
    // IsDynamicQuestExpired()'s own comment. Once this holds, the only
    // legal transition left is ExpireDynamicQuest().
    AlreadyExpired,

    // ExpireDynamicQuest() was called before now >=
    // DynamicQuestInstance::ExpiresAtMs.
    NotYetExpired,

    // CompleteDynamicQuest() was called while Progress < RequiredCount.
    ProgressIncomplete,

    // ApplyDynamicQuestProgress() named a genuinely new (non-duplicate)
    // progress-event identity, but Progress is already == RequiredCount.
    // The event is NOT stored - ConsumedProgressEventIds never grows
    // past RequiredCount entries, keeping both its size and every
    // duplicate-check's cost bounded.
    ProgressAlreadyComplete,

    // ApplyDynamicQuestProgress() was called with a zero progress-event
    // identity - never a legitimate WorldEvent::EventId.
    InvalidProgressEvent,

    // This progress-event identity has already contributed progress to
    // this instance - the same authoritative event can never be counted
    // twice.
    DuplicateProgressEvent
};

char const* ToString(DynamicQuestRejectReason reason);

// The result of one lifecycle transition attempt. Instance is set if and
// only if Reason == None - every rejected transition leaves the caller's
// existing DynamicQuestInstance value completely untouched (every
// transition function below takes its instance by const reference and
// only ever produces a NEW value on success, never a partially-mutated
// one on failure).
//
// Milestone 2.13C2 P2 fix, round 3 (STATIC review): these functions are
// pure and stateless - they judge and transition whatever `instance` a
// caller passes in, with no notion of "the currently stored value" and
// no way to prove where that instance actually came from (an earlier
// fix tried tagging results with a source "revision" number, but that
// only proves what revision the CALLER claimed, not that `instance` was
// ever this registry's own authoritative stored value - a fabricated
// DynamicQuestInstance with a matching Id/Revision but arbitrary other
// fields would pass identically). The actual fix lives in
// DynamicQuestRegistry: its own Accept()/ApplyProgress()/Complete()/
// Fail()/Expire() are the ONLY sanctioned way to produce and commit a
// DynamicQuestTransitionResult against a stored instance, because they
// internally look up and pass their OWN current stored value into these
// functions - a caller never gets to supply the `instance` argument
// itself. Calling these free functions directly (as the tests in this
// file do) is fine for exercising the pure state-machine logic in
// isolation; it is DynamicQuestRegistry's job, not this header's, to
// guarantee authoritative provenance for anything actually committed.
struct DynamicQuestTransitionResult
{
    DynamicQuestRejectReason Reason = DynamicQuestRejectReason::NotAttempted;
    std::optional<DynamicQuestInstance> Instance;

    bool IsAccepted() const
    {
        return Reason == DynamicQuestRejectReason::None && Instance.has_value();
    }
};

// Milestone 2.13C1: constructs a new Offered instance from an already-
// validated QuestProposal (see QuestProposal.h - 2.13B's own output).
// Copies KILL_CREATURE identity/count/expiry fields the state machine
// operates on, plus (Milestone 2.13C4) Title/Description for player-
// facing display and (Milestone 2.13C5) RewardMoneyCopper for turn-in -
// never re-read from the model or recomputed after this point. ExpiresAtMs
// is computed from nowMs + proposal.ExpiryMs with a saturating add - it
// can never wrap around regardless of input values.
// Uses the same DynamicQuestTransitionResult shape as every transition
// below even though there is no prior instance to transition from, so a
// caller has exactly one result type to handle everywhere. Rejects:
// InvalidQuestId (id == DynamicQuestId{0} - see that type's own comment;
// a caller-side allocator never handing out 0 is not proof enough for
// this boundary to rely on).
DynamicQuestTransitionResult OfferDynamicQuest(DynamicQuestId id, QuestProposal const& proposal, uint64 nowMs);

// The single, canonical expiry rule this entire lifecycle domain uses:
// an instance is expired once now >= ExpiresAtMs, independent of its
// current State field. Every transition function below that is not
// ExpireDynamicQuest() itself consults this exact function before doing
// anything else state-specific, so "is this instance expired" is never
// answered two different ways in two different places.
bool IsDynamicQuestExpired(DynamicQuestInstance const& instance, uint64 nowMs);

// Milestone 2.13C5: the single, canonical "has this instance's objective
// been reached" rule - independent of State/expiry, exactly like
// IsDynamicQuestExpired() above (a caller decides whether those also
// apply). CompleteDynamicQuest() below consults this exact function for
// its own ProgressIncomplete check; AIWorldMgr::
// GetDynamicQuestGossipContent() consults it too, to decide
// Kind::ReadyToTurnIn vs. Kind::Active - so "is this quest done" is never
// answered two different ways in two different places.
bool IsDynamicQuestObjectiveComplete(DynamicQuestInstance const& instance);

// Offered -> Active. Binds playerGuid as the instance's owner for the
// rest of its lifetime - there is no later re-assignment. Rejects:
// AlreadyTerminal, InvalidTransition (State != Offered), InvalidPlayer
// (!playerGuid.IsPlayer() - rejects both an empty GUID and any non-
// player entity, e.g. a creature GUID), AlreadyExpired.
DynamicQuestTransitionResult AcceptDynamicQuest(DynamicQuestInstance const& instance, ObjectGuid playerGuid, uint64 nowMs);

// Contributes exactly one unit of progress from one authoritative event,
// saturating at RequiredCount - State stays Active even once Progress
// reaches RequiredCount; CompleteDynamicQuest() is a separate, explicit
// decision. Checked in this order: AlreadyTerminal, InvalidTransition
// (State != Active), AlreadyExpired, InvalidProgressEvent
// (progressEventId == 0), PlayerMismatch (playerGuid !=
// AcceptedByPlayerGuid), DuplicateProgressEvent (progressEventId already
// consumed by this instance), ProgressAlreadyComplete (a genuinely new
// event arriving once Progress == RequiredCount already - never stored,
// so ConsumedProgressEventIds never grows past RequiredCount entries).
// DuplicateProgressEvent is checked before ProgressAlreadyComplete so a
// replay of an already-counted event is always reported as a replay,
// even once Progress is already saturated - never silently accepted as
// "no-op, already complete".
DynamicQuestTransitionResult ApplyDynamicQuestProgress(DynamicQuestInstance const& instance, ObjectGuid playerGuid, uint64 progressEventId, uint64 nowMs);

// Active -> Completed. Rejects: AlreadyTerminal, InvalidTransition
// (State != Active), AlreadyExpired, ProgressIncomplete
// (Progress < RequiredCount).
DynamicQuestTransitionResult CompleteDynamicQuest(DynamicQuestInstance const& instance, uint64 nowMs);

// Active -> Failed. Rejects: AlreadyTerminal, InvalidTransition (State
// != Active - Offered -> Failed is not modeled, see DynamicQuestState.h),
// AlreadyExpired (once the deadline has passed, ExpireDynamicQuest() is
// the only legal transition left, not FailDynamicQuest()).
DynamicQuestTransitionResult FailDynamicQuest(DynamicQuestInstance const& instance, uint64 nowMs);

// Offered|Active -> Expired. Rejects: AlreadyTerminal, NotYetExpired
// (not IsDynamicQuestExpired(instance, nowMs) yet).
DynamicQuestTransitionResult ExpireDynamicQuest(DynamicQuestInstance const& instance, uint64 nowMs);

#endif // AIWORLD_DYNAMICQUESTLIFECYCLE_H
