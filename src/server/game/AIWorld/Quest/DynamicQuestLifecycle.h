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

    // AcceptDynamicQuest() was called with an empty player identity.
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
// Only KILL_CREATURE identity/count/expiry fields this milestone's state
// machine actually operates on are carried over (Title/Description/
// RewardMoneyCopper are not modeled yet - see DynamicQuestInstance's own
// comment). ExpiresAtMs is computed from nowMs + proposal.ExpiryMs with a
// saturating add - it can never wrap around regardless of input values.
// Never fails: proposal is already policy-validated by
// ValidateDynamicTaskCandidate(), and a saturating add has no invalid
// input. Not a "transition" in the DynamicQuestTransitionResult sense -
// there is no prior instance to transition from.
DynamicQuestInstance OfferDynamicQuest(DynamicQuestId id, QuestProposal const& proposal, uint64 nowMs);

// The single, canonical expiry rule this entire lifecycle domain uses:
// an instance is expired once now >= ExpiresAtMs, independent of its
// current State field. Every transition function below that is not
// ExpireDynamicQuest() itself consults this exact function before doing
// anything else state-specific, so "is this instance expired" is never
// answered two different ways in two different places.
bool IsDynamicQuestExpired(DynamicQuestInstance const& instance, uint64 nowMs);

// Offered -> Active. Binds playerGuid as the instance's owner for the
// rest of its lifetime - there is no later re-assignment. Rejects:
// AlreadyTerminal, InvalidTransition (State != Offered), InvalidPlayer
// (playerGuid.IsEmpty()), AlreadyExpired.
DynamicQuestTransitionResult AcceptDynamicQuest(DynamicQuestInstance const& instance, ObjectGuid playerGuid, uint64 nowMs);

// Contributes exactly one unit of progress from one authoritative event,
// saturating at RequiredCount - State stays Active even once Progress
// reaches RequiredCount; CompleteDynamicQuest() is a separate, explicit
// decision. Rejects: AlreadyTerminal, InvalidTransition (State !=
// Active), AlreadyExpired, InvalidProgressEvent (progressEventId == 0),
// PlayerMismatch (playerGuid != AcceptedByPlayerGuid),
// DuplicateProgressEvent (progressEventId already consumed - checked
// AFTER saturation would already have capped it, so a duplicate is
// always rejected even once Progress == RequiredCount, never silently
// accepted as a harmless no-op).
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
