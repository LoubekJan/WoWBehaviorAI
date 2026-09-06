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

#include "Quest/DynamicQuestGossipText.h"

TEST_CASE("FormatDynamicQuestOfferGossipLine", "[DynamicQuestGossipText]")
{
    REQUIRE(FormatDynamicQuestOfferGossipLine("Cull the wolves") == "Dynamic task: Cull the wolves");
}

TEST_CASE("FormatDynamicQuestProgressGossipLine", "[DynamicQuestGossipText]")
{
    REQUIRE(FormatDynamicQuestProgressGossipLine("Cull the wolves", 0, 3) == "Cull the wolves - Progress: 0/3");
    REQUIRE(FormatDynamicQuestProgressGossipLine("Cull the wolves", 2, 3) == "Cull the wolves - Progress: 2/3");
    REQUIRE(FormatDynamicQuestProgressGossipLine("Cull the wolves", 3, 3) == "Cull the wolves - Progress: 3/3");
}

TEST_CASE("FormatDynamicQuestKillProgressMessage", "[DynamicQuestGossipText]")
{
    REQUIRE(FormatDynamicQuestKillProgressMessage(1, 3) == "Dynamic task progress: 1/3");
    REQUIRE(FormatDynamicQuestKillProgressMessage(2, 3) == "Dynamic task progress: 2/3");
    REQUIRE(FormatDynamicQuestKillProgressMessage(3, 3) == "Dynamic task progress: 3/3");
}

TEST_CASE("FormatDynamicQuestObjectiveCompleteMessage", "[DynamicQuestGossipText]")
{
    SECTION("with a giver name")
    {
        REQUIRE(FormatDynamicQuestObjectiveCompleteMessage("Bob") == "Objective complete. Return to Bob.");
    }

    SECTION("without a giver name (could not be re-resolved)")
    {
        REQUIRE(FormatDynamicQuestObjectiveCompleteMessage("") == "Objective complete. Return to the quest giver.");
    }
}

TEST_CASE("FormatDynamicQuestReadyToTurnInGossipLine", "[DynamicQuestGossipText]")
{
    REQUIRE(FormatDynamicQuestReadyToTurnInGossipLine("Cull the wolves") == "Cull the wolves - Objective complete");
}

TEST_CASE("FormatDynamicQuestCompletedMessage", "[DynamicQuestGossipText]")
{
    REQUIRE(FormatDynamicQuestCompletedMessage("Cull the wolves") == "Completed: Cull the wolves.");
}

TEST_CASE("FormatDynamicQuestRewardMessage", "[DynamicQuestGossipText]")
{
    REQUIRE(FormatDynamicQuestRewardMessage(75) == "Reward: 75 copper.");
    REQUIRE(FormatDynamicQuestRewardMessage(0) == "Reward: 0 copper.");
}

TEST_CASE("FormatDynamicQuestCompleteRejectedMessage", "[DynamicQuestGossipText]")
{
    SECTION("RewardMoneyLimit gets its own specific wording")
    {
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::RewardMoneyLimit) ==
            "Unable to turn in dynamic task: you cannot carry that much money.");
    }

    SECTION("every other reason falls back to one generic message")
    {
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::QuestNotFound) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::PlayerInvalid) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::PlayerMismatch) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::GiverMissing) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::GiverChanged) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::GiverUnavailable) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::InteractionRangeInvalid) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::OutOfRange) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::InvalidQuestState) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::AlreadyExpired) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::ProgressIncomplete) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::RewardApplicationFailed) == "Unable to turn in dynamic task.");
        REQUIRE(FormatDynamicQuestCompleteRejectedMessage(DynamicQuestPlayerCompleteReason::CompleteRejected) == "Unable to turn in dynamic task.");
    }
}
