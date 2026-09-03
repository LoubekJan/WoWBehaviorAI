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

#ifndef AIWORLD_DYNAMICTASKVALIDATION_H
#define AIWORLD_DYNAMICTASKVALIDATION_H

#include "Define.h"

struct DynamicTaskCandidate;

// Milestone 2.13B: every way an otherwise fully-accepted DynamicTaskCandidate
// (already passed 2.13A3B's own provenance/staleness/target-binding
// checks) can still fail THIS, the authoritative policy boundary.
// LiveTargetOutOfRange covers both a non-finite live distance and a
// finite one that simply exceeds the draft's own proposed
// MaxRangeYards - see ValidateDynamicTaskCandidate()'s own comment.
enum class DynamicTaskValidationReason : uint8
{
    None = 0,
    UnsupportedObjective,
    TargetBindingMissing,
    TargetBindingMismatch,
    RequiredCountInvalid,
    RangeInvalid,
    ExpiryInvalid,
    RewardInvalid,
    LiveTargetOutOfRange
};

char const* ToString(DynamicTaskValidationReason reason);

// Milestone 2.13B: the server's CURRENT authoritative policy window -
// deliberately never candidate.RequestContext.Limits, which is only the
// policy window that happened to be in effect when the request was
// built (see QuestProposalLimits' own comment: "defense in depth only,
// never authoritative"). Config may have changed since; this validator
// must always judge against policy as of right now.
struct DynamicTaskAuthoritativeLimits
{
    uint32 MaxRequiredCount = 0;
    float MaxRangeYards = 0.0f;
    uint32 MaxExpiryMs = 0;
    uint32 MaxRewardMoneyCopper = 0;
};

// Milestone 2.13B: values the caller has already obtained from a fresh,
// authoritative live-world resolution of the exact target
// candidate.Draft.TargetToken names (see AIWorldMgr::
// OnDynamicTaskCandidateAccepted()) - never from the model, never from
// the original request's own QuestContext/QuestTargetCandidate snapshot.
// A caller must not construct this from anything but a live Creature*
// it just re-resolved itself.
struct DynamicTaskWorldFacts
{
    uint32 TargetEntry = 0;
    uint32 TargetMapId = 0;
    float TargetDistanceYards = 0.0f;
};

// Milestone 2.13B: the authoritative validator - pure, no Creature*/
// Player*/Unit*/Map*/Quest*, no DB access, no logging. Judges
// candidate.Draft against DynamicTaskAuthoritativeLimits (current server
// policy, not anything the request itself carried) and worldFacts (a
// fresh live resolution the caller already performed), in this order:
//   1. candidate.Draft.Objective must be KillCreature - the only
//      objective this milestone's contract supports at all.
//   2. candidate.Draft.TargetToken must resolve to exactly one entry in
//      candidate.Provenance.TargetBindings, and that binding's own
//      Entry/MapId must match worldFacts exactly - the model can never
//      invent a target reference of its own, and the binding it named
//      must actually describe the live target the caller resolved.
//   3. RequiredCount must be > 0 and <= limits.MaxRequiredCount.
//   4. MaxRangeYards must be finite, > 0, and <= limits.MaxRangeYards.
//   5. ExpiryMs must be > 0 and <= limits.MaxExpiryMs.
//   6. RewardMoneyCopper must be <= limits.MaxRewardMoneyCopper.
//   7. worldFacts.TargetDistanceYards must be finite and
//      <= candidate.Draft.MaxRangeYards - a schema-valid, under-server-
//      cap proposal is still rejected if the target the model actually
//      picked is, in reality, already farther away than the range it
//      itself proposed; such a task could never be completed as
//      described from the moment it would be created.
// Returns None only once every one of these has held. Never authorizes
// gameplay by itself - the caller decides what a None result means.
DynamicTaskValidationReason ValidateDynamicTaskCandidate(
    DynamicTaskCandidate const& candidate,
    DynamicTaskAuthoritativeLimits const& limits,
    DynamicTaskWorldFacts const& worldFacts);

#endif // AIWORLD_DYNAMICTASKVALIDATION_H
