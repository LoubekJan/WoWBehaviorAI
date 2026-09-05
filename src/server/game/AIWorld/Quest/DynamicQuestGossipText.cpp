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

#include "DynamicQuestGossipText.h"

std::string FormatDynamicQuestOfferGossipLine(std::string const& title)
{
    return "Dynamic task: " + title;
}

std::string FormatDynamicQuestProgressGossipLine(std::string const& title, uint32 progress, uint32 requiredCount)
{
    return title + " - Progress: " + std::to_string(progress) + "/" + std::to_string(requiredCount);
}

std::string FormatDynamicQuestKillProgressMessage(uint32 progress, uint32 requiredCount)
{
    return "Dynamic task progress: " + std::to_string(progress) + "/" + std::to_string(requiredCount);
}

std::string FormatDynamicQuestObjectiveCompleteMessage(std::string const& giverName)
{
    if (giverName.empty())
        return "Objective complete. Return to the quest giver.";

    return "Objective complete. Return to " + giverName + ".";
}

std::string FormatDynamicQuestReadyToTurnInGossipLine(std::string const& title)
{
    return title + " - Objective complete";
}

std::string FormatDynamicQuestCompletedMessage(std::string const& title)
{
    return "Completed: " + title + ".";
}

std::string FormatDynamicQuestRewardMessage(uint32 rewardMoneyCopper)
{
    return "Reward: " + std::to_string(rewardMoneyCopper) + " copper.";
}
