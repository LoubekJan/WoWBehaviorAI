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

#include <limits>

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

    // A fully self-consistent candidate + limits + worldFacts triple that
    // ValidateDynamicTaskCandidate() must accept - every negative test
    // below starts from this and breaks exactly one thing.
    DynamicTaskCandidate MakeValidCandidate()
    {
        DynamicTaskCandidate candidate;
        candidate.RequestId = 123;
        candidate.AcceptedAtMs = 10000;

        candidate.Provenance.Agent.Value = 42;
        candidate.Provenance.SnapshotSequence = 77;
        candidate.Provenance.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
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

    DynamicTaskWorldFacts MakeValidWorldFacts()
    {
        DynamicTaskWorldFacts worldFacts;
        worldFacts.TargetEntry = 2002;
        worldFacts.TargetMapId = 0;
        worldFacts.TargetDistanceYards = 15.0f;
        return worldFacts;
    }
}

TEST_CASE("ValidateDynamicTaskCandidate accepts a fully valid candidate", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

    REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::None);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects an unsupported objective", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    candidate.Draft.Objective = QuestObjectiveType::Invalid;
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

    REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::UnsupportedObjective);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects an unknown target token", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    candidate.Draft.TargetToken = 999;
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

    REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::TargetBindingMissing);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects a binding/live-target Entry mismatch", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
    worldFacts.TargetEntry = 9999; // binding still says 2002

    REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::TargetBindingMismatch);
}

TEST_CASE("ValidateDynamicTaskCandidate rejects a binding/live-target MapId mismatch", "[DynamicTaskValidation]")
{
    DynamicTaskCandidate candidate = MakeValidCandidate();
    DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
    DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
    worldFacts.TargetMapId = 1; // binding still says map 0

    REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::TargetBindingMismatch);
}

TEST_CASE("ValidateDynamicTaskCandidate enforces required_count against current policy", "[DynamicTaskValidation]")
{
    SECTION("zero is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.RequiredCount = 0;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RequiredCountInvalid);
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
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RequiredCountInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.RequiredCount = limits.MaxRequiredCount;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces max_range_yards against current policy", "[DynamicTaskValidation]")
{
    SECTION("zero is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = 0.0f;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = 0.0f;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("NaN is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = std::numeric_limits<float>::quiet_NaN();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("infinity is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.MaxRangeYards = std::numeric_limits<float>::infinity();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("over the current authoritative limit is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.MaxRangeYards = limits.MaxRangeYards + 1.0f;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = limits.MaxRangeYards; // within the proposed (but invalid) range

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RangeInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.MaxRangeYards = limits.MaxRangeYards;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = limits.MaxRangeYards;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces expiry_ms against current policy", "[DynamicTaskValidation]")
{
    SECTION("zero is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        candidate.Draft.ExpiryMs = 0;
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::ExpiryInvalid);
    }

    SECTION("over the current authoritative limit is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.ExpiryMs = limits.MaxExpiryMs + 1;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::ExpiryInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.ExpiryMs = limits.MaxExpiryMs;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces reward_money_copper against current policy", "[DynamicTaskValidation]")
{
    SECTION("over the current authoritative limit is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        candidate.Draft.RewardMoneyCopper = limits.MaxRewardMoneyCopper + 1;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::RewardInvalid);
    }

    SECTION("exactly at the current limit is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        limits.MaxRewardMoneyCopper = 500;
        candidate.Draft.RewardMoneyCopper = limits.MaxRewardMoneyCopper;
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ValidateDynamicTaskCandidate enforces the live target distance against the draft's own proposed range", "[DynamicTaskValidation]")
{
    SECTION("a NaN live distance is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = std::numeric_limits<float>::quiet_NaN();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("an infinite live distance is rejected")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = std::numeric_limits<float>::infinity();

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("a live distance beyond the draft's own proposed range is rejected even though it is within server policy")
    {
        // The target the model picked is, in reality, already farther
        // away than the range the draft itself proposes - a schema-valid,
        // under-server-cap task that could never be completed as described.
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = candidate.Draft.MaxRangeYards + 1.0f;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::LiveTargetOutOfRange);
    }

    SECTION("a live distance exactly at the draft's own proposed range is accepted")
    {
        DynamicTaskCandidate candidate = MakeValidCandidate();
        DynamicTaskAuthoritativeLimits limits = MakeValidLimits();
        DynamicTaskWorldFacts worldFacts = MakeValidWorldFacts();
        worldFacts.TargetDistanceYards = candidate.Draft.MaxRangeYards;

        REQUIRE(ValidateDynamicTaskCandidate(candidate, limits, worldFacts) == DynamicTaskValidationReason::None);
    }
}

TEST_CASE("ToString(DynamicTaskValidationReason) covers every enumerator", "[DynamicTaskValidation]")
{
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::None)) == "NONE");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::UnsupportedObjective)) == "UNSUPPORTED_OBJECTIVE");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::TargetBindingMissing)) == "TARGET_BINDING_MISSING");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::TargetBindingMismatch)) == "TARGET_BINDING_MISMATCH");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::RequiredCountInvalid)) == "REQUIRED_COUNT_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::RangeInvalid)) == "RANGE_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::ExpiryInvalid)) == "EXPIRY_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::RewardInvalid)) == "REWARD_INVALID");
    REQUIRE(std::string(ToString(DynamicTaskValidationReason::LiveTargetOutOfRange)) == "LIVE_TARGET_OUT_OF_RANGE");
}
