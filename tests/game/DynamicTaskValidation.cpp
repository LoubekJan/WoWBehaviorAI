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

#include "Inference/DynamicTaskValidation.h"
#include "Inference/DynamicTaskCandidate.h"
#include "Inference/QuestContractLimits.h"

#include <limits>
#include <string>

namespace
{
    QuestTargetBinding MakeBinding(uint32 token, uint32 entry, uint32 mapId)
    {
        QuestTargetBinding binding;
        binding.Token = token;
        binding.Entry = entry;
        binding.MapId = mapId;
        binding.Guid = ObjectGuid::Create<HighGuid::Unit>(entry, token);
        return binding;
    }

    // A fully self-consistent candidate + limits + facts triple that
    // ValidateDynamicTaskCandidate() must accept - every negative test
    // below starts from this and breaks exactly one thing.
    DynamicTaskCandidate MakeValidCandidate()
    {
        DynamicTaskCandidate candidate;
        candidate.RequestId = 123;
        candidate.AcceptedAtMs = 10000;

        candidate.RequestContext.Agent.Value = 42;
        candidate.RequestContext.SnapshotSequence = 77;
        candidate.RequestContext.Problem.Type = WorldEventType::CreatureKilled;
        candidate.RequestContext.Problem.ActorEntry = 1001;
        candidate.RequestContext.Problem.TargetEntry = 2002;
        candidate.RequestContext.Problem.MapId = 0;
        candidate.RequestContext.Problem.AgeMs = 500;

        candidate.Provenance.Agent.Value = 42;
        candidate.Provenance.SnapshotSequence = 77;
        candidate.Provenance.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        candidate.Provenance.SourceEventId = 9001;
        candidate.Provenance.SourceEventType = WorldEventType::CreatureKilled;
        candidate.Provenance.TargetBindings.push_back(MakeBinding(1, 2002, 0));

        candidate.Draft.Objective = QuestObjectiveType::KillCreature;
        candidate.Draft.TargetToken = 1;
        candidate.Draft.RequiredCount = 3;
        candidate.Draft.MaxRangeYards = 40.0f;
        candidate.Draft.ExpiryMs = 200000;
        candidate.Draft.RewardMoneyCopper = 0;
        candidate.Draft.Title = "Cull the wolves";
        candidate.Draft.Description = "Thin the wolf pack near the road.";

        return candidate;
    }

    DynamicTaskAuthoritativeLimits MakeValidLimits()
    {
        DynamicTaskAuthoritativeLimits limits;
        limits.MaxRequiredCount = 5;
        limits.MaxRangeYards = 60.0f;
        limits.MaxExpiryMs = 300000;
        limits.MaxRewardMoneyCopper = 0;
        return limits;
    }

    DynamicTaskAuthoritativeFacts MakeValidFacts()
    {
        DynamicTaskAuthoritativeFacts facts;
        facts.Source.Type = WorldEventType::CreatureKilled;
        facts.Source.ActorEntry = 1001;
        facts.Source.TargetEntry = 2002;
        facts.Source.MapId = 0;
        facts.Target.Entry = 2002;
        facts.Target.MapId = 0;
        facts.Target.GiverToTargetDistanceYards = 15.0f;
        return facts;
    }
}

TEST_CASE("ValidateDynamicTaskCandidate accepts a fully valid candidate", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

    DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
    REQUIRE(result.Reason == DynamicTaskValidationReason::None);
    REQUIRE(result.IsAccepted());
    REQUIRE(result.Proposal.has_value());

    QuestProposal const& proposal = *result.Proposal;
    REQUIRE(proposal.Giver.Value == candidate.Provenance.Agent.Value);
    REQUIRE(proposal.GiverRuntimeGuid == candidate.Provenance.RuntimeGuid);
    REQUIRE(proposal.SourceEventId == candidate.Provenance.SourceEventId);
    REQUIRE(proposal.SourceEventType == candidate.Provenance.SourceEventType);
    REQUIRE(proposal.Objective == QuestObjectiveType::KillCreature);
    REQUIRE(proposal.TargetToken == 1);
    REQUIRE(proposal.TargetGuid == candidate.Provenance.TargetBindings[0].Guid);
    REQUIRE(proposal.TargetEntry == 2002);
    REQUIRE(proposal.TargetMapId == 0);
    REQUIRE(proposal.RequiredCount == candidate.Draft.RequiredCount);
    REQUIRE(proposal.MaxRangeYards == candidate.Draft.MaxRangeYards);
    REQUIRE(proposal.ExpiryMs == candidate.Draft.ExpiryMs);
    REQUIRE(proposal.RewardMoneyCopper == candidate.Draft.RewardMoneyCopper);
    REQUIRE(proposal.Title == candidate.Draft.Title);
    REQUIRE(proposal.Description == candidate.Draft.Description);
}

TEST_CASE("DynamicTaskValidationResult defaults to rejected, never to an accepted None", "[DynamicTaskValidation]")
{
    DynamicTaskValidationResult result;
    REQUIRE(result.Reason == DynamicTaskValidationReason::NotValidated);
    REQUIRE(result.Reason != DynamicTaskValidationReason::None);
    REQUIRE_FALSE(result.IsAccepted());
    REQUIRE_FALSE(result.Proposal.has_value());
}

TEST_CASE("ValidateDynamicTaskCandidate rejects an unsupported objective", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    candidate.Draft.Objective = QuestObjectiveType::Invalid;
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

    DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
    REQUIRE(result.Reason == DynamicTaskValidationReason::UnsupportedObjective);
    REQUIRE_FALSE(result.Proposal.has_value());
}

TEST_CASE("ValidateDynamicTaskCandidate rejects a source problem mismatch", "[DynamicTaskValidation]")
{
    SECTION("event type mismatch")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Source.Type = WorldEventType::NPCInjured;

        DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
        REQUIRE(result.Reason == DynamicTaskValidationReason::SourceProblemMismatch);
        REQUIRE_FALSE(result.Proposal.has_value());
    }

    SECTION("actor entry mismatch")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Source.ActorEntry = 9999;

        DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
        REQUIRE(result.Reason == DynamicTaskValidationReason::SourceProblemMismatch);
    }

    SECTION("target entry mismatch")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Source.TargetEntry = 9999;

        DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
        REQUIRE(result.Reason == DynamicTaskValidationReason::SourceProblemMismatch);
    }

    SECTION("map mismatch")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Source.MapId = 1;

        DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
        REQUIRE(result.Reason == DynamicTaskValidationReason::SourceProblemMismatch);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate rejects an unknown target token", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    candidate.Draft.TargetToken = 999;
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

    DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
    REQUIRE(result.Reason == DynamicTaskValidationReason::TargetBindingMissing);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects an ambiguous (duplicate-token) binding list", "[DynamicTaskValidation]")
{
    // The header's own contract: TargetToken must resolve to EXACTLY one
    // binding - a second binding sharing the same token must never be
    // silently resolved by "take the first match".
    DynamicTaskCandidate candidate = MakeValidCandidate();
    candidate.Provenance.TargetBindings.push_back(MakeBinding(1, 3003, 0));
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

    DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
    REQUIRE(result.Reason == DynamicTaskValidationReason::TargetBindingMissing);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects a binding/live-target Entry mismatch", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
    facts.Target.Entry = 9999; // binding still says 2002

    DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
    REQUIRE(result.Reason == DynamicTaskValidationReason::TargetBindingMismatch);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects a binding/live-target MapId mismatch", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
    facts.Target.MapId = 1; // binding still says map 0

    DynamicTaskValidationResult result = ValidateDynamicTaskCandidate(candidate, limits, facts);
    REQUIRE(result.Reason == DynamicTaskValidationReason::TargetBindingMismatch);
}

TEST_CASE("ValidateDynamicTaskCandidate enforces required_count against current policy", "[DynamicTaskValidation]")
{
    SECTION("zero is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.RequiredCount = 0;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RequiredCountInvalid);
    }

    SECTION("over the CURRENT authoritative limit is rejected even though it was under the request-time limit")
    {
        // Review intent for 2.13B: candidate.Draft was already accepted by
        // 2.13A3B against a QuestContext::Limits snapshot of
        // MaxRequiredCount=5 captured when the request was built. Policy
        // has since tightened to 2 - this validator must judge against
        // THAT current value, never trust the request's own stale limit.
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.RequiredCount = 3; // was valid under the old limit of 5
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        limits.MaxRequiredCount = 2; // current policy is now stricter
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RequiredCountInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.RequiredCount = limits.MaxRequiredCount;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces max_range_yards against current policy", "[DynamicTaskValidation]")
{
    SECTION("zero is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = 0.0f;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = 0.0f;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("negative is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = -1.0f;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("NaN is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = std::numeric_limits<float>::quiet_NaN();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("infinity is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = std::numeric_limits<float>::infinity();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("over the current authoritative limit is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.MaxRangeYards = limits.MaxRangeYards + 1.0f;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = limits.MaxRangeYards; // within the proposed (but invalid) range

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.MaxRangeYards = limits.MaxRangeYards;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = limits.MaxRangeYards;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }

    SECTION("a non-finite authoritative policy value fails closed instead of silently admitting or never bounding a draft")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        SECTION("NaN policy limit")
        {
            DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
            limits.MaxRangeYards = std::numeric_limits<float>::quiet_NaN();
            REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
        }

        SECTION("infinite policy limit")
        {
            DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
            limits.MaxRangeYards = std::numeric_limits<float>::infinity();
            REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
        }

        SECTION("zero policy limit")
        {
            DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
            limits.MaxRangeYards = 0.0f;
            REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RangeInvalid);
        }
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces expiry_ms against current policy", "[DynamicTaskValidation]")
{
    SECTION("zero is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.ExpiryMs = 0;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::ExpiryInvalid);
    }

    SECTION("over the current authoritative limit is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.ExpiryMs = limits.MaxExpiryMs + 1;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::ExpiryInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.ExpiryMs = limits.MaxExpiryMs;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces reward_money_copper against current policy", "[DynamicTaskValidation]")
{
    SECTION("over the current authoritative limit is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.RewardMoneyCopper = limits.MaxRewardMoneyCopper + 1;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::RewardInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        limits.MaxRewardMoneyCopper = 500;
        candidate.Draft.RewardMoneyCopper = limits.MaxRewardMoneyCopper;
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate independently re-validates Title/Description", "[DynamicTaskValidation]")
{
    SECTION("empty title is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Title = "";
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::TextInvalid);
    }

    SECTION("empty description is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Description = "";
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::TextInvalid);
    }

    SECTION("title over QuestContractMaxTitleLength is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Title = std::string(QuestContractMaxTitleLength + 1, 'a');
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::TextInvalid);
    }

    SECTION("description over QuestContractMaxDescriptionLength is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Description = std::string(QuestContractMaxDescriptionLength + 1, 'a');
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::TextInvalid);
    }

    SECTION("a control character (embedded newline) in the title is rejected even though it fits the byte cap")
    {
        // The wire codec only ever enforces a byte-length cap - an escaped
        // "\n" in a JSON string is a perfectly valid, short string as far
        // as that check is concerned. 2.13B must independently reject it.
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Title = "Cull the\nwolves";
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::TextInvalid);
    }

    SECTION("a control character in the description is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Description = std::string("Thin the pack\tnear the road.");
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::TextInvalid);
    }

    SECTION("exactly at the length cap, with no control characters, is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.Title = std::string(QuestContractMaxTitleLength, 'a');
        candidate.Draft.Description = std::string(QuestContractMaxDescriptionLength, 'b');
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces the live giver-to-target distance against the draft's own proposed range", "[DynamicTaskValidation]")
{
    SECTION("a negative live distance is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = -1.0f;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("a NaN live distance is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = std::numeric_limits<float>::quiet_NaN();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("an infinite live distance is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = std::numeric_limits<float>::infinity();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("a live distance beyond the draft's own proposed range is rejected even though it is within server policy")
    {
        // The target the model picked is, in reality, already farther
        // away than the range the draft itself proposes - a schema-valid,
        // under-server-cap task that could never be completed as described.
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = candidate.Draft.MaxRangeYards + 1.0f;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("a live distance exactly at the draft's own proposed range is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = candidate.Draft.MaxRangeYards;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }

    SECTION("a live distance of exactly zero is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskAuthoritativeFacts facts = MakeValidFacts();
        facts.Target.GiverToTargetDistanceYards = 0.0f;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, facts).Reason == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ToString(DynamicTaskValidationReason) covers every enumerator", "[DynamicTaskValidation]")
{
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::NotValidated)) == "NOT_VALIDATED");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::None)) == "NONE");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::UnsupportedObjective)) == "UNSUPPORTED_OBJECTIVE");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::SourceProblemMismatch)) == "SOURCE_PROBLEM_MISMATCH");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::TargetBindingMissing)) == "TARGET_BINDING_MISSING");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::TargetBindingMismatch)) == "TARGET_BINDING_MISMATCH");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::RequiredCountInvalid)) == "REQUIRED_COUNT_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::RangeInvalid)) == "RANGE_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::ExpiryInvalid)) == "EXPIRY_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::RewardInvalid)) == "REWARD_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::TextInvalid)) == "TEXT_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::LiveTargetOutOfRange)) == "LIVE_TARGET_OUT_OF_RANGE");
}
