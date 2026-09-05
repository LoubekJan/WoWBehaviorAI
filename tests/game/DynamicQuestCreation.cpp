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

#include "Quest/DynamicQuestCreation.h"
#include "Inference/QuestProposal.h"

#include <limits>

namespace
{
    QuestProposal MakeValidProposal()
    {
        QuestProposal proposal;
        proposal.Giver.Value = 42;
        proposal.GiverRuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        proposal.SourceEventId = 9001;
        proposal.SourceEventType = WorldEventType::CreatureKilled;
        proposal.Objective = QuestObjectiveType::KillCreature;
        proposal.TargetToken = 1;
        proposal.TargetGuid = ObjectGuid::Create<HighGuid::Unit>(2002, 1);
        proposal.TargetEntry = 2002;
        proposal.TargetMapId = 0;
        proposal.RequiredCount = 3;
        proposal.MaxRangeYards = 40.0f;
        proposal.ExpiryMs = 200000;
        proposal.RewardMoneyCopper = 0;
        proposal.Title = "Cull the wolves";
        proposal.Description = "Thin the wolf pack near the road.";
        return proposal;
    }

    DynamicQuestGiverFacts MakeValidGiverFacts(QuestProposal const& proposal)
    {
        DynamicQuestGiverFacts facts;
        facts.RecordExists = true;
        facts.Materialized = true;
        facts.AIWorldControlled = true;
        facts.Alive = true;
        facts.RuntimeGuid = proposal.GiverRuntimeGuid;
        return facts;
    }

    DynamicQuestTargetFacts MakeValidTargetFacts(QuestProposal const& proposal)
    {
        DynamicQuestTargetFacts facts;
        facts.Resolved = true;
        facts.Alive = true;
        facts.Attackable = true;
        facts.Entry = proposal.TargetEntry;
        facts.MapId = proposal.TargetMapId;
        facts.GiverToTargetDistanceYards = 15.0f; // well within proposal.MaxRangeYards (40.0f)
        return facts;
    }
}

TEST_CASE("CheckDynamicQuestCreateApplicability accepts fully-matching live facts", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
    DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);

    REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::None);
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects a missing giver record", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver; // RecordExists = false
    DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);

    REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverMissing);
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects a changed giver runtime incarnation", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
    giver.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 999); // different incarnation
    DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);

    REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverChanged);
}

TEST_CASE("CheckDynamicQuestCreateApplicability treats no-live-giver-resolved as GiverChanged, not GiverMissing", "[DynamicQuestCreation]")
{
    // RecordExists is true (the AgentRecord itself was found) but no live
    // Creature was ever resolved, so RuntimeGuid stays empty - an empty
    // GUID never legitimately matches a real captured RuntimeGuid.
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
    giver.RuntimeGuid = ObjectGuid::Empty;
    DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);

    REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverChanged);
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects giver unavailability", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);

    SECTION("not Materialized")
    {
        DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
        giver.Materialized = false;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverUnavailable);
    }

    SECTION("not AIWorldControlled")
    {
        DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
        giver.AIWorldControlled = false;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverUnavailable);
    }

    SECTION("not alive")
    {
        DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
        giver.Alive = false;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverUnavailable);
    }
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects an unresolved target", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);
    DynamicQuestTargetFacts target; // Resolved = false

    REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetMissing);
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects a changed target identity", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);

    SECTION("entry mismatch")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.Entry = 9999;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetChanged);
    }

    SECTION("map mismatch")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.MapId = 1;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetChanged);
    }
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects a live giver-to-target distance out of range", "[DynamicQuestCreation]")
{
    // Same live-range invariant ValidateDynamicTaskCandidate() already
    // enforced in 2.13B - re-proven here since that was a point-in-time
    // check, not a standing guarantee.
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);

    SECTION("NaN distance")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.GiverToTargetDistanceYards = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetOutOfRange);
    }

    SECTION("infinite distance")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.GiverToTargetDistanceYards = std::numeric_limits<float>::infinity();
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetOutOfRange);
    }

    SECTION("negative distance")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.GiverToTargetDistanceYards = -1.0f;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetOutOfRange);
    }

    SECTION("beyond proposal.MaxRangeYards")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.GiverToTargetDistanceYards = proposal.MaxRangeYards + 1.0f;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetOutOfRange);
    }

    SECTION("exactly at proposal.MaxRangeYards is accepted")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.GiverToTargetDistanceYards = proposal.MaxRangeYards;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::None);
    }
}

TEST_CASE("CheckDynamicQuestCreateApplicability rejects target unavailability", "[DynamicQuestCreation]")
{
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver = MakeValidGiverFacts(proposal);

    SECTION("not alive")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.Alive = false;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetUnavailable);
    }

    SECTION("not attackable")
    {
        DynamicQuestTargetFacts target = MakeValidTargetFacts(proposal);
        target.Attackable = false;
        REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::TargetUnavailable);
    }
}

TEST_CASE("Giver identity/incarnation checks take priority over target facts", "[DynamicQuestCreation]")
{
    // An unresolved/garbage target must never surface as the rejection
    // reason when the giver itself is already the problem.
    QuestProposal proposal = MakeValidProposal();
    DynamicQuestGiverFacts giver; // RecordExists = false
    DynamicQuestTargetFacts target; // also unresolved

    REQUIRE(CheckDynamicQuestCreateApplicability(proposal, giver, target) == DynamicQuestCreateReason::GiverMissing);
}

TEST_CASE("ToString(DynamicQuestCreateReason) covers every enumerator", "[DynamicQuestCreation]")
{
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::NotAttempted)) == "NOT_ATTEMPTED");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::None)) == "NONE");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::RegistryFull)) == "REGISTRY_FULL");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::GiverMissing)) == "GIVER_MISSING");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::GiverChanged)) == "GIVER_CHANGED");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::GiverUnavailable)) == "GIVER_UNAVAILABLE");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::TargetMissing)) == "TARGET_MISSING");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::TargetChanged)) == "TARGET_CHANGED");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::TargetUnavailable)) == "TARGET_UNAVAILABLE");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::TargetOutOfRange)) == "TARGET_OUT_OF_RANGE");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::IdExhausted)) == "ID_EXHAUSTED");
    REQUIRE(std::string(ToString(DynamicQuestCreateReason::OfferRejected)) == "OFFER_REJECTED");
}

TEST_CASE("DynamicQuestCreateResult defaults to rejected, never to an accepted None", "[DynamicQuestCreation]")
{
    DynamicQuestCreateResult result;
    REQUIRE(result.Reason == DynamicQuestCreateReason::NotAttempted);
    REQUIRE(result.Reason != DynamicQuestCreateReason::None);
    REQUIRE_FALSE(result.IsCreated());
    REQUIRE_FALSE(result.Id.has_value());
}
