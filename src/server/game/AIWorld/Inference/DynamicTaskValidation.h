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
#include "Event/WorldEventType.h"
#include "QuestProposal.h"

#include <optional>

struct DynamicTaskCandidate;

// Milestone 2.13B: every way an otherwise fully-accepted DynamicTaskCandidate
// (already passed 2.13A3B's own provenance/staleness/target-binding
// checks) can still fail THIS, the authoritative policy boundary.
// NotValidated is the enum's zero value on purpose - a default-constructed
// DynamicTaskValidationResult must read as rejected, never as if it had
// already been judged and passed.
enum class DynamicTaskValidationReason : uint8
{
    NotValidated = 0,
    None,
    UnsupportedObjective,
    SourceProblemMismatch,
    TargetBindingMissing,
    TargetBindingMismatch,
    RequiredCountInvalid,
    RangeInvalid,
    ExpiryInvalid,
    RewardInvalid,
    TextInvalid,
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

// A fresh, live re-derivation of the exact same source memory
// candidate.Provenance's SourceEventId/SourceCorrelationId/
// SourceOccurredAtMs/SourceEventType already identify - built by the
// caller from the SAME memory record HandleDynamicTaskResponse() already
// found while proving that identity match (never re-looked-up here).
// Only the payload fields QuestProblemContext also carries: identity and
// age are 2.13A3B's own job, not this validator's.
struct DynamicTaskSourceFacts
{
    WorldEventType Type = WorldEventType::CreatureKilled;
    uint32 ActorEntry = 0;
    uint32 TargetEntry = 0;
    uint32 MapId = 0;
};

// A fresh, live re-derivation of the exact target
// candidate.Draft.TargetToken names - built by the caller from the same
// live Creature* HandleDynamicTaskResponse() already resolved and
// checked (alive, entry/map match, attackable), never re-resolved here.
// GiverToTargetDistanceYards is named explicitly (not "TargetDistance")
// so it can never be confused with the draft's own proposed
// MaxRangeYards - the two are compared against each other, not the same
// value under two names.
struct DynamicTaskTargetFacts
{
    uint32 Entry = 0;
    uint32 MapId = 0;
    float GiverToTargetDistanceYards = 0.0f;
};

struct DynamicTaskAuthoritativeFacts
{
    DynamicTaskSourceFacts Source;
    DynamicTaskTargetFacts Target;
};

// The result of one ValidateDynamicTaskCandidate() call. Proposal is set
// if and only if Reason == None - IsAccepted() checks both, so a caller
// can never end up treating a rejected result's leftover/default Proposal
// as real, and never ends up treating a NotValidated (default-constructed,
// never-run) result as accepted either.
struct DynamicTaskValidationResult
{
    DynamicTaskValidationReason Reason = DynamicTaskValidationReason::NotValidated;
    std::optional<QuestProposal> Proposal;

    bool IsAccepted() const
    {
        return Reason == DynamicTaskValidationReason::None && Proposal.has_value();
    }
};

// Milestone 2.13B: the authoritative validator - pure, no Creature*/
// Player*/Unit*/Map*/Quest*, no DB access, no logging. Judges
// candidate.Draft against DynamicTaskAuthoritativeLimits (current server
// policy, not anything the request itself carried) and facts (a fresh
// live re-derivation the caller already performed), in this order:
//   1. candidate.Draft.Objective must be KillCreature - the only
//      objective this milestone's contract supports at all.
//   2. facts.Source must match candidate.RequestContext.Problem exactly
//      (Type/ActorEntry/TargetEntry/MapId) - the world problem that
//      justified this request in the first place must still describe
//      the same situation right now, not just still exist.
//   3. candidate.Draft.TargetToken must resolve to exactly one entry in
//      candidate.Provenance.TargetBindings (a token naming zero or more
//      than one binding is treated as unresolved, never as "pick one"),
//      and that binding's own Entry/MapId must match facts.Target
//      exactly - the model can never invent a target reference of its
//      own, and the binding it named must actually describe the live
//      target the caller resolved.
//   4. RequiredCount must be > 0 and <= limits.MaxRequiredCount.
//   5. limits.MaxRangeYards and MaxRangeYards must both be finite and
//      > 0, and MaxRangeYards <= limits.MaxRangeYards - a misconfigured
//      non-finite or non-positive server policy value fails closed here
//      rather than silently admitting (NaN) or never bounding (+Inf)
//      every draft.
//   6. ExpiryMs must be > 0 and <= limits.MaxExpiryMs.
//   7. RewardMoneyCopper must be <= limits.MaxRewardMoneyCopper.
//   8. Title/Description must be non-empty, within
//      QuestContractMaxTitleLength/QuestContractMaxDescriptionLength,
//      and free of ASCII control characters (0x00-0x1F, 0x7F) - a
//      structurally valid-length string can still carry an embedded
//      newline or other formatting character the wire codec's own byte-
//      length check would never catch.
//   9. facts.Target.GiverToTargetDistanceYards must be finite,
//      non-negative, and <= candidate.Draft.MaxRangeYards - a schema-
//      valid, under-server-cap proposal is still rejected if the target
//      the model actually picked is, in reality, already farther away
//      than the range it itself proposed; such a task could never be
//      completed as described from the moment it would be created.
// Returns a result with Reason == None and a populated Proposal only
// once every one of these has held. Never authorizes gameplay by
// itself - the caller decides what an accepted result means.
DynamicTaskValidationResult ValidateDynamicTaskCandidate(
    DynamicTaskCandidate const& candidate,
    DynamicTaskAuthoritativeLimits const& limits,
    DynamicTaskAuthoritativeFacts const& facts);

#endif // AIWORLD_DYNAMICTASKVALIDATION_H
