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

#include "Quest/DynamicQuestPlayerCompletion.h"
#include "Quest/DynamicQuestInstance.h"

#include <limits>

namespace
{
    ObjectGuid const kPlayerGuid = ObjectGuid::Create<HighGuid::Player>(uint32(1));

    DynamicQuestInstance MakeReadyToTurnInInstance()
    {
        DynamicQuestInstance instance;
        instance.Id = DynamicQuestId{7};
        instance.State = DynamicQuestState::Active;
        instance.Giver.Value = 42;
        instance.GiverRuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 555);
        instance.RequiredCount = 3;
        instance.Progress = 3;
        instance.RewardMoneyCopper = 75;
        instance.CreatedAtMs = 10000;
        instance.ExpiresAtMs = 210000;
        instance.AcceptedByPlayerGuid = kPlayerGuid;
        return instance;
    }

    DynamicQuestPlayerCompleteFacts MakeValidPlayerFacts()
    {
        DynamicQuestPlayerCompleteFacts facts;
        facts.IsPlayerGuid = true;
        facts.Resolved = true;
        facts.Alive = true;
        facts.MapId = 0;
        facts.Money = 1000;
        return facts;
    }

    DynamicQuestGiverCompleteFacts MakeValidGiverFacts(DynamicQuestInstance const& instance)
    {
        DynamicQuestGiverCompleteFacts facts;
        facts.RecordExists = true;
        facts.Materialized = true;
        facts.AIWorldControlled = true;
        facts.Alive = true;
        facts.RuntimeGuid = instance.GiverRuntimeGuid;
        facts.MapId = 0;
        return facts;
    }

    constexpr float MaxRangeYards = 10.0f;
    constexpr uint32 MaxMoneyAmount = 1000000;

    // Milestone 2.13C5 P2 fix, round 2 (STATIC review): well before
    // MakeReadyToTurnInInstance()'s own ExpiresAtMs (210000), so every
    // existing test above stays unaffected by the newly-added expiry
    // check unless it deliberately sets nowMs at/after ExpiresAtMs itself.
    constexpr uint64 NowMs = 10000;
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability accepts fully-matching live facts at RequiredCount", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::None);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a missing/unresolved player", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    SECTION("not a player guid")
    {
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.IsPlayerGuid = false;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::PlayerInvalid);
    }

    SECTION("not resolved (offline/missing)")
    {
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.Resolved = false;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::PlayerInvalid);
    }
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a dead player", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    player.Alive = false;
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::PlayerInvalid);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a different player than the one who accepted", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    ObjectGuid someoneElse = ObjectGuid::Create<HighGuid::Player>(uint32(2));
    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, someoneElse, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::PlayerMismatch);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a missing giver record", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver; // RecordExists = false

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::GiverMissing);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a changed giver runtime incarnation", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
    giver.RuntimeGuid = ObjectGuid::Create<HighGuid::Unit>(1001, 999); // different incarnation

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::GiverChanged);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects giver unavailability", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();

    SECTION("not Materialized")
    {
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        giver.Materialized = false;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::GiverUnavailable);
    }

    SECTION("not AIWorldControlled")
    {
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        giver.AIWorldControlled = false;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::GiverUnavailable);
    }

    SECTION("not alive")
    {
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        giver.Alive = false;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::GiverUnavailable);
    }
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a map mismatch", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    player.MapId = 1;
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
    giver.MapId = 0;

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 0.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::OutOfRange);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects an out-of-range live distance", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    SECTION("NaN distance")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, std::numeric_limits<float>::quiet_NaN(), MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::OutOfRange);
    }

    SECTION("infinite distance")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, std::numeric_limits<float>::infinity(), MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::OutOfRange);
    }

    SECTION("negative distance")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, -1.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::OutOfRange);
    }

    SECTION("beyond the configured max range")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, MaxRangeYards + 0.1f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::OutOfRange);
    }

    SECTION("exactly at the configured max range is accepted")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, MaxRangeYards, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::None);
    }
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability fails closed on an invalid server policy max range", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    SECTION("NaN max range")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, std::numeric_limits<float>::quiet_NaN(), MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InteractionRangeInvalid);
    }

    SECTION("infinite max range")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, std::numeric_limits<float>::infinity(), MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InteractionRangeInvalid);
    }

    SECTION("sub-minimum (0.5) max range")
    {
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 0.4f, 0.5f, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InteractionRangeInvalid);
    }
}

// ---------------------------------------------------------------------
// Milestone 2.13C5 P2 fix, round 2 (STATIC review): State/expiry must be
// rejected here, BEFORE AIWorldMgr::CompleteDynamicQuestForPlayer() ever
// calls Player::ModifyMoney() - not left solely to
// DynamicQuestRegistry::Complete()'s own (still independently kept)
// re-check.
// ---------------------------------------------------------------------

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a non-Active instance as InvalidQuestState", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();

    SECTION("Offered")
    {
        DynamicQuestInstance instance = MakeReadyToTurnInInstance();
        instance.State = DynamicQuestState::Offered;
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InvalidQuestState);
    }

    SECTION("Completed")
    {
        DynamicQuestInstance instance = MakeReadyToTurnInInstance();
        instance.State = DynamicQuestState::Completed;
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InvalidQuestState);
    }

    SECTION("Failed")
    {
        DynamicQuestInstance instance = MakeReadyToTurnInInstance();
        instance.State = DynamicQuestState::Failed;
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InvalidQuestState);
    }

    SECTION("Expired")
    {
        DynamicQuestInstance instance = MakeReadyToTurnInInstance();
        instance.State = DynamicQuestState::Expired;
        DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::InvalidQuestState);
    }
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects an Active instance whose deadline has passed as AlreadyExpired", "[DynamicQuestPlayerCompletion]")
{
    // The exact "expired in the gap between gossip and the click"
    // scenario the fix closes - State is still literally Active (nobody
    // called ExpireDynamicQuest() yet), but nowMs has already crossed
    // ExpiresAtMs.
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, instance.ExpiresAtMs - 1) == DynamicQuestPlayerCompleteReason::None);
    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, instance.ExpiresAtMs) == DynamicQuestPlayerCompleteReason::AlreadyExpired);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects an incomplete objective", "[DynamicQuestPlayerCompletion]")
{
    // Milestone 2.13C5: DynamicQuestRegistry::Complete() (via
    // CompleteDynamicQuest()) independently re-checks this exact same
    // condition - deliberately redundant, see this reason's own comment
    // in DynamicQuestPlayerCompletion.h.
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    instance.Progress = 2; // 2 of 3
    DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::ProgressIncomplete);
}

TEST_CASE("CheckDynamicQuestPlayerCompleteApplicability rejects a reward that would exceed the money cap", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    instance.RewardMoneyCopper = 100;
    DynamicQuestGiverCompleteFacts giver = MakeValidGiverFacts(instance);

    SECTION("player money + reward exceeds the cap")
    {
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.Money = MaxMoneyAmount - 50; // + 100 reward overflows
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::RewardMoneyLimit);
    }

    SECTION("player money already at the cap, even a zero reward is fine")
    {
        DynamicQuestInstance zeroRewardInstance = instance;
        zeroRewardInstance.RewardMoneyCopper = 0;
        DynamicQuestGiverCompleteFacts zeroRewardGiver = MakeValidGiverFacts(zeroRewardInstance);
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.Money = MaxMoneyAmount;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(zeroRewardInstance, kPlayerGuid, player, zeroRewardGiver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::None);
    }

    SECTION("player money already OVER the cap (should not happen, but must not underflow into a false accept)")
    {
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.Money = MaxMoneyAmount + 1;
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::RewardMoneyLimit);
    }

    // Milestone 2.13C5 P2 fix (STATIC review): Player::ModifyMoney()'s own
    // real success condition is GetMoney() < MAX_MONEY_AMOUNT - amount,
    // i.e. player.Money + reward must be STRICTLY LESS than the cap - an
    // earlier version of this check used <=, which let exactly
    // player.Money + reward == maxMoneyAmount pass this preflight only
    // for the real ModifyMoney() call to still refuse it afterward (by
    // which point sScriptMgr->OnPlayerMoneyChanged() had already fired -
    // see AIWorldMgr::CompleteDynamicQuestForPlayer()'s own comment).
    SECTION("player money + reward exactly equals the cap is rejected")
    {
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.Money = MaxMoneyAmount - 100; // + 100 reward == exactly the cap
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::RewardMoneyLimit);
    }

    SECTION("player money + reward one below the cap is accepted")
    {
        DynamicQuestPlayerCompleteFacts player = MakeValidPlayerFacts();
        player.Money = MaxMoneyAmount - 101; // + 100 reward == cap - 1
        REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, 5.0f, MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::None);
    }
}

TEST_CASE("Player eligibility checks take priority over giver/progress/money facts", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestInstance instance = MakeReadyToTurnInInstance();
    instance.Progress = 0; // also incomplete
    DynamicQuestPlayerCompleteFacts player; // IsPlayerGuid = false
    DynamicQuestGiverCompleteFacts giver; // also RecordExists = false

    REQUIRE(CheckDynamicQuestPlayerCompleteApplicability(instance, kPlayerGuid, player, giver, std::numeric_limits<float>::infinity(), MaxRangeYards, MaxMoneyAmount, NowMs) == DynamicQuestPlayerCompleteReason::PlayerInvalid);
}

TEST_CASE("ToString(DynamicQuestPlayerCompleteReason) covers every enumerator", "[DynamicQuestPlayerCompletion]")
{
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::NotAttempted)) == "NOT_ATTEMPTED");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::None)) == "NONE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::QuestNotFound)) == "QUEST_NOT_FOUND");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::PlayerInvalid)) == "PLAYER_INVALID");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::PlayerMismatch)) == "PLAYER_MISMATCH");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::GiverMissing)) == "GIVER_MISSING");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::GiverChanged)) == "GIVER_CHANGED");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::GiverUnavailable)) == "GIVER_UNAVAILABLE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::InteractionRangeInvalid)) == "INTERACTION_RANGE_INVALID");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::OutOfRange)) == "OUT_OF_RANGE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::InvalidQuestState)) == "INVALID_QUEST_STATE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::AlreadyExpired)) == "ALREADY_EXPIRED");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::ProgressIncomplete)) == "PROGRESS_INCOMPLETE");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::RewardMoneyLimit)) == "REWARD_MONEY_LIMIT");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::RewardApplicationFailed)) == "REWARD_APPLICATION_FAILED");
    REQUIRE(std::string(ToString(DynamicQuestPlayerCompleteReason::CompleteRejected)) == "COMPLETE_REJECTED");
}

TEST_CASE("DynamicQuestPlayerCompleteResult defaults to rejected, never to a completed None", "[DynamicQuestPlayerCompletion]")
{
    DynamicQuestPlayerCompleteResult result;
    REQUIRE(result.Reason == DynamicQuestPlayerCompleteReason::NotAttempted);
    REQUIRE(result.Reason != DynamicQuestPlayerCompleteReason::None);
    REQUIRE_FALSE(result.IsCompleted());
}
