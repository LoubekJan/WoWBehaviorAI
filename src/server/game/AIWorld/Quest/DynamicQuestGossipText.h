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

#ifndef AIWORLD_DYNAMICQUESTGOSSIPTEXT_H
#define AIWORLD_DYNAMICQUESTGOSSIPTEXT_H

#include "Define.h"

#include <string>

// Milestone 2.13C4: pure, testable text formatting for the dynamic quest
// gossip UI and client-facing progress messages. No Player*/Creature*,
// no TrinityCore gossip/chat API of any kind - AIWorldCreatureAI/
// AIWorldMgr call these to get the exact string, then hand it to the
// real client-facing API themselves (AddGossipItemFor()/
// ChatHandler::PSendSysMessage()). Keeping the actual wording here, not
// inline at each call site, means every player-facing string this
// milestone produces has an explicit, reviewable, unit-testable
// definition in one place.

// The gossip line shown for a freshly Offered quest nobody has accepted
// yet - e.g. "Dynamic task: Cull the wolves".
std::string FormatDynamicQuestOfferGossipLine(std::string const& title);

// The gossip line shown when the accepting player returns to the giver
// with an Active, not-yet-complete quest - e.g.
// "Cull the wolves - Progress: 2/3".
std::string FormatDynamicQuestProgressGossipLine(std::string const& title, uint32 progress, uint32 requiredCount);

// The chat message sent immediately after an authoritative kill
// contributes progress - e.g. "Dynamic task progress: 2/3".
std::string FormatDynamicQuestKillProgressMessage(uint32 progress, uint32 requiredCount);

// The chat message sent immediately once progress reaches RequiredCount.
// giverName is the giver's live display name at that moment (see
// AIWorldMgr::ProcessDynamicQuestKillProgress()'s own comment for why it
// is re-resolved fresh rather than captured at Offer() time) - an empty
// giverName (giver could not be re-resolved right now) falls back to a
// generic phrase rather than producing "Return to ." verbatim.
std::string FormatDynamicQuestObjectiveCompleteMessage(std::string const& giverName);

// Milestone 2.13C5: the gossip line shown once the accepting player has
// reached RequiredCount but has not yet turned the quest in - e.g.
// "Cull the wolves - Objective complete". Deliberately distinct from
// FormatDynamicQuestProgressGossipLine() above (which would otherwise
// print "Cull the wolves - Progress: 3/3") - a player revisiting the
// giver at this point should be told there is something to DO (turn in),
// not just shown a number that happens to have stopped changing.
std::string FormatDynamicQuestReadyToTurnInGossipLine(std::string const& title);

// The chat message sent immediately once AIWorldMgr::
// CompleteDynamicQuestForPlayer() actually commits Active -> Completed -
// e.g. "Completed: Cull the wolves.".
std::string FormatDynamicQuestCompletedMessage(std::string const& title);

// The chat message sent alongside FormatDynamicQuestCompletedMessage()
// only when rewardMoneyCopper > 0 - e.g. "Reward: 75 copper.". Never
// called for a zero reward (see CompleteDynamicQuestForPlayer()'s own
// call site) rather than printing "Reward: 0 copper." for a quest the
// model happened not to attach a reward to.
std::string FormatDynamicQuestRewardMessage(uint32 rewardMoneyCopper);

#endif // AIWORLD_DYNAMICQUESTGOSSIPTEXT_H
