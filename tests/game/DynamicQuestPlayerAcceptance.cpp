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

#include "Quest/DynamicQuestPlayerAcceptance.h"
#include "Quest/DynamicQuestInstance.h"

#include <limits>

namespace
{
    DynamicQuestInstance MakeOfferedInstance()
    {
        DynamicQuestInstance instance;
        instance.Id = DynamicQuestId{7};
        instance.State = DynamicQuestState::Offered;
        instance.Giver.Value = 42;
        instance.GiverRuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        instance.RequiredCount = 3;
        instance.CreatedAtMs = 10000;
        instance.ExpiresAtMs = 210000;
        return instance;
    }

    DynamicQuestPlayerFacts MakeValidPlayerFacts()
    {
        DynamicQuestPlayerFacts facts;
        facts.IsPlayerGuid = true;
        facts.Resolved = true;
        facts.Alive = true;
        facts.MapId = 0;
        return facts;
    }

    DynamicQuestGiverAcceptFacts MakeValidGiverFacts(DynamicQuestInstance const& instance)
    {
        DynamicQuestGiverAcceptFacts facts;
        facts.RecordExists = true;
        facts.Materialized = true;
        facts.AIWorldControlled = true;
        facts.Alive = true;
        facts.RuntimeGuid = instance.GiverRuntimeGuid;
        facts.MapId = 0;
        return facts;
    }

    constexpr float MaxRangeYards = 10.0f;
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability accepts fully-matching live facts", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::None);
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects a missing/unresolved player", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);

    SECTION("not a player guid")
    {
        DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
        player.IsPlayerGuid = false;
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::PlayerInvalid);
    }

    SECTION("not resolved (offline/missing)")
    {
        DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
        player.Resolved = false;
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::PlayerInvalid);
    }
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects a dead player", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    player.Alive = false;
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::PlayerInvalid);
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects a missing giver record", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverAcceptFacts giver; // RecordExists = false

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::GiverMissing);
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects a changed giver runtime incarnation", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);
    giver.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 999); // different incarnation

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::GiverChanged);
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability treats no-live-giver-resolved as GiverChanged, not GiverMissing", "[DynamicQuestPlayerAcceptance]")
{
    // RecordExists is true (the AgentRecord itself was found) but no live
    // Creature was ever resolved, so RuntimeGuid stays empty - an empty
    // GUID never legitimately matches a real captured RuntimeGuid.
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);
    giver.RuntimeGuid = ObjectGuid::Empty;

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::GiverChanged);
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects giver unavailability", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();

    SECTION("not Materialized")
    {
        DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);
        giver.Materialized = false;
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::GiverUnavailable);
    }

    SECTION("not AIWorldControlled")
    {
        DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);
        giver.AIWorldControlled = false;
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::GiverUnavailable);
    }

    SECTION("not alive")
    {
        DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);
        giver.Alive = false;
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::GiverUnavailable);
    }
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects a map mismatch", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    player.MapId = 1;
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);
    giver.MapId = 0;

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 0.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::OutOfRange);
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability rejects an out-of-range live distance", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);

    SECTION("NaN distance")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, std::numeric_limits<float>::quiet_NaN(), MaxRangeYards) == DynamicQuestPlayerAcceptReason::OutOfRange);
    }

    SECTION("infinite distance")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, std::numeric_limits<float>::infinity(), MaxRangeYards) == DynamicQuestPlayerAcceptReason::OutOfRange);
    }

    SECTION("negative distance")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, -1.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::OutOfRange);
    }

    SECTION("beyond the configured max range")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, MaxRangeYards + 0.1f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::OutOfRange);
    }

    SECTION("exactly at the configured max range is accepted")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, MaxRangeYards, MaxRangeYards) == DynamicQuestPlayerAcceptReason::None);
    }

    SECTION("exactly zero is accepted")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 0.0f, MaxRangeYards) == DynamicQuestPlayerAcceptReason::None);
    }
}

TEST_CASE("CheckDynamicQuestPlayerAcceptApplicability fails closed on an invalid server policy max range", "[DynamicQuestPlayerAcceptance]")
{
    // Milestone 2.13C3 P2 fix (STATIC review): a misconfigured/corrupted
    // maxInteractionRangeYards must never silently admit every distance
    // (NaN) or every distance on the map (+Infinity) - it fails closed
    // as its own distinct reason, checked independently of the measured
    // live distance itself.
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverAcceptFacts giver = MakeValidGiverFacts(instance);

    SECTION("NaN max range")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, std::numeric_limits<float>::quiet_NaN()) == DynamicQuestPlayerAcceptReason::InteractionRangeInvalid);
    }

    SECTION("infinite max range")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 5.0f, std::numeric_limits<float>::infinity()) == DynamicQuestPlayerAcceptReason::InteractionRangeInvalid);
    }

    SECTION("zero max range")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 0.0f, 0.0f) == DynamicQuestPlayerAcceptReason::InteractionRangeInvalid);
    }

    SECTION("sub-minimum (0.5) max range")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 0.4f, 0.5f) == DynamicQuestPlayerAcceptReason::InteractionRangeInvalid);
    }

    SECTION("exactly at the 1.0 floor is accepted as policy-valid")
    {
        REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, 1.0f, 1.0f) == DynamicQuestPlayerAcceptReason::None);
    }
}

TEST_CASE("Player eligibility checks take priority over giver/range facts", "[DynamicQuestPlayerAcceptance]")
{
    // An invalid giver/range must never surface as the rejection reason
    // when the player itself is already the problem.
    DynamicQuestInstance instance = MakeOfferedInstance();
    DynamicQuestPlayerFacts player; // IsPlayerGuid = false
    DynamicQuestGiverAcceptFacts giver; // also RecordExists = false

    REQUIRE(CheckDynamicQuestPlayerAcceptApplicability(instance, player, giver, std::numeric_limits<float>::infinity(), MaxRangeYards) == DynamicQuestPlayerAcceptReason::PlayerInvalid);
}

TEST_CASE("ToString(DynamicQuestPlayerAcceptReason) covers every enumerator", "[DynamicQuestPlayerAcceptance]")
{
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::NotAttempted)) == "NOT_ATTEMPTED");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::None)) == "NONE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::QuestNotFound)) == "QUEST_NOT_FOUND");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::PlayerInvalid)) == "PLAYER_INVALID");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::GiverMissing)) == "GIVER_MISSING");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::GiverChanged)) == "GIVER_CHANGED");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::GiverUnavailable)) == "GIVER_UNAVAILABLE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::OutOfRange)) == "OUT_OF_RANGE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::InteractionRangeInvalid)) == "INTERACTION_RANGE_INVALID");
    REQUIRE(std::string(ToString(DynamicQuestPlayerAcceptReason::AcceptRejected)) == "ACCEPT_REJECTED");
}

TEST_CASE("DynamicQuestPlayerAcceptResult defaults to rejected, never to an accepted None", "[DynamicQuestPlayerAcceptance]")
{
    DynamicQuestPlayerAcceptResult result;
    REQUIRE(result.Reason == DynamicQuestPlayerAcceptReason::NotAttempted);
    REQUIRE(result.Reason != DynamicQuestPlayerAcceptReason::None);
    REQUIRE_FALSE(result.IsAccepted());
}
