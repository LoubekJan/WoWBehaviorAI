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

#ifndef AIWORLD_DYNAMICQUESTCREATION_H
#define AIWORLD_DYNAMICQUESTCREATION_H

#include "Define.h"
#include "DynamicQuestId.h"
#include "ObjectGuid.h"

#include <optional>

struct QuestProposal;

// Milestone 2.13C2: every way an already-validated QuestProposal (2.13B's
// own output) can still fail to become a real Offered DynamicQuestInstance.
// NotAttempted is the zero value on purpose, matching every other reason
// enum in this milestone chain (DynamicTaskValidationReason::NotValidated,
// DynamicQuestRejectReason::NotAttempted) - a default-constructed
// DynamicQuestCreateResult must read as rejected, never accepted.
enum class DynamicQuestCreateReason : uint8
{
    NotAttempted = 0,
    None,

    // The giver AgentId no longer resolves to an AgentRecord at all.
    GiverMissing,

    // The giver resolves, but its CURRENT live-Creature GUID no longer
    // matches proposal.GiverRuntimeGuid - a despawn/respawn incarnation
    // swap since 2.13B's own validation (or no live Creature at all,
    // which never legitimately matches a real captured RuntimeGuid).
    GiverChanged,

    // The giver's identity still matches, but it is not currently usable
    // (not Materialized, not AIWorldControlled, or not alive).
    GiverUnavailable,

    // proposal.TargetGuid no longer resolves to a live Creature at all.
    TargetMissing,

    // The target resolves, but its CURRENT Entry/MapId no longer match
    // proposal.TargetEntry/TargetMapId.
    TargetChanged,

    // The target's identity still matches, but it is not currently
    // usable (not alive, or no longer a valid attack target for giver).
    TargetUnavailable,

    // AllocateDynamicQuestId()/AdvanceDynamicQuestIdCounter() could not
    // mint a new id - the process-lifetime uint64 counter is exhausted.
    IdExhausted,

    // OfferDynamicQuest() itself rejected the freshly allocated
    // DynamicQuestId - should be unreachable in practice, since a freshly
    // minted id is never 0 (see AdvanceDynamicQuestIdCounter's own
    // comment); defense in depth only.
    OfferRejected,

    // DynamicQuestRegistry::Add() itself rejected the offer - should be
    // unreachable in practice, since a freshly minted id colliding would
    // mean the allocator itself is broken; defense in depth only.
    RegistryRejected
};

char const* ToString(DynamicQuestCreateReason reason);

// Milestone 2.13C2: a fresh, live re-derivation of the giver's identity
// and availability - the caller (AIWorldMgr) must gather this from a
// live AgentRecord/Creature* re-resolution, never from anything
// QuestProposal itself already carries (that is exactly the stale value
// this check exists to catch drift against).
struct DynamicQuestGiverFacts
{
    bool RecordExists = false;
    bool Materialized = false;
    bool AIWorldControlled = false;
    bool Alive = false;

    // The live Creature's GUID, ObjectGuid::Empty if none was resolved.
    ObjectGuid RuntimeGuid;
};

// Milestone 2.13C2: a fresh, live re-derivation of the target's identity
// and availability - same rule as DynamicQuestGiverFacts: gathered from a
// live Creature* re-resolution (via proposal.TargetGuid), never trusted
// from the proposal's own Entry/MapId fields directly.
struct DynamicQuestTargetFacts
{
    bool Resolved = false;
    bool Alive = false;
    bool Attackable = false;
    uint32 Entry = 0;
    uint32 MapId = 0;
};

// Milestone 2.13C2: the pure applicability re-check. A validated
// QuestProposal is not gameplay authorization (see QuestProposal's own
// comment); this is the "fresh live re-resolution" every consumer of one
// owes itself before acting on it, exactly like 2.13B owed one to
// 2.13A3B's own acceptance. No Creature*/Player*/Map* - only values the
// caller already resolved. Checked in this order: giver identity
// (RecordExists), giver incarnation (RuntimeGuid), giver availability
// (Materialized/AIWorldControlled/Alive), target identity (Resolved),
// target incarnation (Entry/MapId), target availability (Alive/
// Attackable) - identity/incarnation checks take priority over
// availability checks, since a currently-usable but WRONG entity is
// still wrong. Returns None only once every one of those has held.
DynamicQuestCreateReason CheckDynamicQuestCreateApplicability(
    QuestProposal const& proposal,
    DynamicQuestGiverFacts const& giver,
    DynamicQuestTargetFacts const& target);

// The result of one dynamic-quest creation attempt (see AIWorldMgr::
// CreateDynamicQuestOffer()). Id is set if and only if Reason == None.
struct DynamicQuestCreateResult
{
    DynamicQuestCreateReason Reason = DynamicQuestCreateReason::NotAttempted;
    std::optional<DynamicQuestId> Id;

    bool IsCreated() const
    {
        return Reason == DynamicQuestCreateReason::None && Id.has_value();
    }
};

#endif // AIWORLD_DYNAMICQUESTCREATION_H
