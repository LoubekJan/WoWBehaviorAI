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

#ifndef AIWORLD_DYNAMICQUESTPLAYERCOMPLETION_H
#define AIWORLD_DYNAMICQUESTPLAYERCOMPLETION_H

#include "Define.h"
#include "ObjectGuid.h"

struct DynamicQuestInstance;

// Milestone 2.13C5: every way a player's request to turn in a registry-
// owned Active, objective-complete dynamic quest can be rejected before
// ever reaching DynamicQuestRegistry::Complete() itself. Deliberately a
// SEPARATE boundary from DynamicQuestPlayerAcceptance.h's own 2.13C3
// Accept-side reasons/facts, even though several checks below mirror
// that file's shape closely - accept and complete are two different
// authorizations against two different quest states (Offered vs.
// Active-and-done), and a log line's reason= should unambiguously say
// which boundary actually rejected a given attempt, never a value that
// could have come from either. NotAttempted is the zero value on
// purpose, matching every other reason enum in this milestone chain - a
// default-constructed DynamicQuestPlayerCompleteResult must read as
// rejected, never accepted.
enum class DynamicQuestPlayerCompleteReason : uint8
{
    NotAttempted = 0,
    None,

    // No DynamicQuestInstance with this id is currently registered.
    QuestNotFound,

    // playerGuid is not a real, live, in-world, alive player.
    PlayerInvalid,

    // playerGuid is a real, live player, but not the one this specific
    // quest's own AcceptedByPlayerGuid is bound to - a second player can
    // never turn in (or be paid the reward for) someone else's accepted
    // quest.
    PlayerMismatch,

    // The giver AgentId this quest was offered from no longer resolves
    // to an AgentRecord at all.
    GiverMissing,

    // The giver resolves, but its CURRENT live-Creature GUID no longer
    // matches DynamicQuestInstance::GiverRuntimeGuid - a despawn/respawn
    // incarnation swap since this quest was offered (or no live Creature
    // at all, which never legitimately matches a real captured
    // RuntimeGuid).
    GiverChanged,

    // The giver's identity still matches, but it is not currently usable
    // (not Materialized, not AIWorldControlled, or not alive).
    GiverUnavailable,

    // The server's OWN configured maxInteractionRangeYards policy value
    // is itself non-finite or below the smallest sane interaction
    // distance (< 1.0 yard) - see 2.13C3's own InteractionRangeInvalid
    // comment (DynamicQuestPlayerAcceptance.h) for the full reasoning;
    // this is that exact same fail-closed policy sanity check, just
    // re-applied at this separate boundary.
    InteractionRangeInvalid,

    // Player and giver are not on the same map, or the live distance
    // between them is non-finite, negative, or exceeds the server's
    // configured interaction range - turning in from across the map (or
    // the world) is never legitimate.
    OutOfRange,

    // !IsDynamicQuestObjectiveComplete(instance) (DynamicQuestLifecycle.h)
    // - nothing to turn in yet, regardless of State.
    // DynamicQuestRegistry::Complete() (via CompleteDynamicQuest())
    // independently re-checks this exact same canonical condition on its
    // own - deliberately redundant, the same way this whole function's
    // checks are never a substitute for what the registry itself still
    // verifies, just a way to give the caller a more specific reason than
    // a generic CompleteRejected fold.
    ProgressIncomplete,

    // instance.RewardMoneyCopper would push the player's own money past
    // the server's MAX_MONEY_AMOUNT - never silently clamped or
    // dropped; the turn-in itself is refused so the quest stays
    // Active/ReadyToTurnIn for a later attempt instead of silently
    // shortchanging the player. maxMoneyAmount is passed in by the
    // caller (TrinityCore's own Player.h constant) rather than included
    // here, keeping this file free of any Player.h/live-object
    // dependency.
    RewardMoneyLimit,

    // DynamicQuestRegistry::Complete() itself rejected - see the log
    // line's own reason= for the precise underlying
    // DynamicQuestRejectReason (AlreadyTerminal/InvalidTransition/
    // AlreadyExpired/ProgressIncomplete). Should be unreachable given
    // every check above already passed on the same world thread with
    // the same nowMs - see AIWorldMgr::CompleteDynamicQuestForPlayer()'s
    // own comment for the money-compensation this triggers if reward
    // money was already granted before this was somehow still reached.
    CompleteRejected
};

char const* ToString(DynamicQuestPlayerCompleteReason reason);

// Milestone 2.13C5: a fresh, live re-derivation of the completing
// player's identity, availability, and current money - the caller
// (AIWorldMgr) must gather this from a live ObjectAccessor::FindPlayer()
// re-resolution, never cached or trusted from anything earlier in the
// request.
struct DynamicQuestPlayerCompleteFacts
{
    bool IsPlayerGuid = false;
    bool Resolved = false;
    bool Alive = false;
    uint32 MapId = 0;
    uint32 Money = 0;
};

// Milestone 2.13C5: a fresh, live re-derivation of the giver's identity
// and availability - same rule as DynamicQuestPlayerCompleteFacts:
// gathered from a live AgentRecord/Creature* re-resolution, never
// trusted from anything DynamicQuestInstance itself already carries
// (that is exactly the stale value this check exists to catch drift
// against).
struct DynamicQuestGiverCompleteFacts
{
    bool RecordExists = false;
    bool Materialized = false;
    bool AIWorldControlled = false;
    bool Alive = false;
    ObjectGuid RuntimeGuid;
    uint32 MapId = 0;
};

// Milestone 2.13C5: the pure applicability re-check a player's turn-in
// request owes before DynamicQuestRegistry::Complete() may even be
// attempted - the same "fresh live re-resolution" discipline
// CheckDynamicQuestPlayerAcceptApplicability() already established in
// 2.13C3. No Player*/Creature*/Map* - only values the caller already
// resolved, plus maxMoneyAmount (see RewardMoneyLimit's own comment).
// Checked in this order: player eligibility (IsPlayerGuid/Resolved/
// Alive), player binding (playerGuid == instance.AcceptedByPlayerGuid),
// giver identity (RecordExists), giver incarnation (RuntimeGuid), giver
// availability (Materialized/AIWorldControlled/Alive), same map,
// maxInteractionRangeYards policy sanity, live interaction range,
// objective progress, reward affordability - identity/incarnation/
// binding checks take priority over availability/range/progress/money,
// since a currently-usable but WRONG player or giver is still wrong
// regardless of whether the objective is done or the reward could be
// paid. The money check uses 64-bit arithmetic
// (uint64(player.Money) + uint64(instance.RewardMoneyCopper) >
// uint64(maxMoneyAmount)) rather than a same-width subtraction, so it
// can never itself underflow/wrap into a false negative. Deliberately
// does not re-check DynamicQuestInstance::State/expiry itself - that
// stays exclusively DynamicQuestRegistry::Complete()'s own job (via
// CompleteDynamicQuest()), never duplicated here.
DynamicQuestPlayerCompleteReason CheckDynamicQuestPlayerCompleteApplicability(
    DynamicQuestInstance const& instance,
    ObjectGuid playerGuid,
    DynamicQuestPlayerCompleteFacts const& player,
    DynamicQuestGiverCompleteFacts const& giver,
    float playerToGiverDistanceYards,
    float maxInteractionRangeYards,
    uint32 maxMoneyAmount);

// The result of one AIWorldMgr::CompleteDynamicQuestForPlayer() attempt.
struct DynamicQuestPlayerCompleteResult
{
    DynamicQuestPlayerCompleteReason Reason = DynamicQuestPlayerCompleteReason::NotAttempted;

    bool IsCompleted() const
    {
        return Reason == DynamicQuestPlayerCompleteReason::None;
    }
};

#endif // AIWORLD_DYNAMICQUESTPLAYERCOMPLETION_H
