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

#ifndef AIWORLD_DYNAMICQUESTPLAYERACCEPTANCE_H
#define AIWORLD_DYNAMICQUESTPLAYERACCEPTANCE_H

#include "Define.h"
#include "ObjectGuid.h"

struct DynamicQuestInstance;

// Milestone 2.13C3: every way a player's request to accept a registry-
// owned Offered dynamic quest can be rejected before ever reaching
// DynamicQuestRegistry::Accept() itself. NotAttempted is the zero value
// on purpose, matching every other reason enum in this milestone chain -
// a default-constructed DynamicQuestPlayerAcceptResult must read as
// rejected, never accepted.
enum class DynamicQuestPlayerAcceptReason : uint8
{
    NotAttempted = 0,
    None,

    // No DynamicQuestInstance with this id is currently registered.
    QuestNotFound,

    // playerGuid is not a real, live, in-world, alive player - covers
    // ObjectGuid::IsPlayer() being false, the player not currently being
    // online/resolved, and the player being dead.
    PlayerInvalid,

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

    // Player and giver are not on the same map, or the live distance
    // between them is non-finite, negative, or exceeds the server's
    // configured interaction range - accepting a quest from across the
    // map (or the world) is never legitimate.
    OutOfRange,

    // DynamicQuestRegistry::Accept() itself rejected - see the log
    // line's own reason= for the precise underlying
    // DynamicQuestRejectReason (AlreadyTerminal/InvalidTransition/
    // AlreadyExpired/InvalidPlayer/PlayerMismatch).
    AcceptRejected
};

char const* ToString(DynamicQuestPlayerAcceptReason reason);

// Milestone 2.13C3: a fresh, live re-derivation of the accepting
// player's identity and availability - the caller (AIWorldMgr) must
// gather this from a live ObjectAccessor::FindPlayer() re-resolution,
// never cached or trusted from anything earlier in the request.
struct DynamicQuestPlayerFacts
{
    bool IsPlayerGuid = false;
    bool Resolved = false;
    bool Alive = false;
    uint32 MapId = 0;
};

// Milestone 2.13C3: a fresh, live re-derivation of the giver's identity
// and availability - same rule as DynamicQuestPlayerFacts: gathered from
// a live AgentRecord/Creature* re-resolution, never trusted from
// anything DynamicQuestInstance itself already carries (that is exactly
// the stale value this check exists to catch drift against).
struct DynamicQuestGiverAcceptFacts
{
    bool RecordExists = false;
    bool Materialized = false;
    bool AIWorldControlled = false;
    bool Alive = false;
    ObjectGuid RuntimeGuid;
    uint32 MapId = 0;
};

// Milestone 2.13C3: the pure applicability re-check a player's accept
// request owes before DynamicQuestRegistry::Accept() may even be
// attempted - the same "fresh live re-resolution" discipline
// CheckDynamicQuestCreateApplicability() already established in 2.13C2.
// No Player*/Creature*/Map* - only values the caller already resolved.
// Checked in this order: player eligibility (IsPlayerGuid/Resolved/
// Alive), giver identity (RecordExists), giver incarnation (RuntimeGuid),
// giver availability (Materialized/AIWorldControlled/Alive), same map,
// live interaction range (playerToGiverDistanceYards finite, non-
// negative, <= maxInteractionRangeYards) - identity/incarnation checks
// take priority over availability/range checks, since a currently-usable
// but WRONG entity is still wrong. Deliberately does not re-check
// DynamicQuestInstance::State/expiry itself - that stays exclusively
// DynamicQuestRegistry::Accept()'s own job (via AcceptDynamicQuest()),
// never duplicated here.
DynamicQuestPlayerAcceptReason CheckDynamicQuestPlayerAcceptApplicability(
    DynamicQuestInstance const& instance,
    DynamicQuestPlayerFacts const& player,
    DynamicQuestGiverAcceptFacts const& giver,
    float playerToGiverDistanceYards,
    float maxInteractionRangeYards);

// The result of one AIWorldMgr::AcceptDynamicQuestForPlayer() attempt.
struct DynamicQuestPlayerAcceptResult
{
    DynamicQuestPlayerAcceptReason Reason = DynamicQuestPlayerAcceptReason::NotAttempted;

    bool IsAccepted() const
    {
        return Reason == DynamicQuestPlayerAcceptReason::None;
    }
};

#endif // AIWORLD_DYNAMICQUESTPLAYERACCEPTANCE_H
