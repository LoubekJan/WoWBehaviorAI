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
