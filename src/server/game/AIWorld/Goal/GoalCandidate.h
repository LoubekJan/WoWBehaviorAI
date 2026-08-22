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

#ifndef AIWORLD_GOALCANDIDATE_H
#define AIWORLD_GOALCANDIDATE_H

#include "GoalType.h"

// Milestone 2.7A: a possible goal an agent's current NeedsState suggests,
// nothing more - not an active goal, no status/timeout/success condition
// (that's 2.7B's ActiveGoal), and not itself a decision or action (Action
// API is a separate 2.8). Pure value: no AgentId, Creature*, Map*,
// AgentRecord*, or DB.
struct GoalCandidate
{
    GoalType Type = GoalType::GetFood;
    GoalPriority Priority = GoalPriority::Normal;
    GoalSource Source = GoalSource::Needs;
    float Utility = 0.0f;
};

#endif // AIWORLD_GOALCANDIDATE_H
