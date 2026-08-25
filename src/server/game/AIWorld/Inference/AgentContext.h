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

#ifndef AIWORLD_AGENTCONTEXT_H
#define AIWORLD_AGENTCONTEXT_H

#include "Action/ActionType.h"
#include "Agent/AgentSnapshot.h"
#include "DecisionMemory.h"
#include "Define.h"
#include "Goal/ActiveGoal.h"
#include "Needs/NeedsState.h"
#include <optional>
#include <vector>

// Milestone 2.9A: the complete "what is this agent allowed to know right
// now" context handed to ai-server for one decision - everything
// DecisionRequest sends, and nothing else. Self already carries Agent
// identity and SnapshotSequence (the state token a decision answers - see
// AIWorldMgr::Update()'s stale-response check), so they aren't repeated as
// separate fields here. Pure value: no Creature*/Player*/Map* anywhere,
// same rule as every other AIWorld DTO. Built fresh every
// CaptureAgentContext() call from data AIWorldMgr already has on hand
// (AgentRecord::Needs/ActiveGoalState, this tick's own
// MemoryRetrieval::Retrieve() result) - AgentContext itself never fetches
// or computes anything.
struct AgentContext
{
    AgentSnapshot Self;
    NeedsState Needs;

    // Milestone 2.7B1: empty when the agent currently has no ActiveGoal.
    std::optional<ActiveGoal> Goal;

    // Milestone 2.5C/2.9A P2 fix: this tick's Top-N MemoryRetrieval::Retrieve()
    // result, already relevance-sorted, truncated, and sanitized down to
    // DecisionMemory (see DecisionMemory.h for why RetrievedMemory itself
    // isn't wire-safe as-is) - ai-server gets exactly what was selected,
    // never the full short/long-term memory pool, and never a raw
    // ObjectGuid/SpawnId.
    std::vector<DecisionMemory> RelevantMemories;

    // Explicitly enumerated by the world thread rather than left for
    // ai-server to assume - today the full ActionType catalog AIWorld
    // implements (Flee/MoveTo/Eat, see ActionType.h), not yet filtered
    // down to what's actually valid for this agent right now (that
    // filtering stays ActionSystem::Validate()'s job on the way back in -
    // see AIWorldMgr::Update()'s 2.9A comment for why a decision response
    // still isn't turned into an ActionRequest at all yet).
    std::vector<ActionType> AvailableActions;
};

#endif // AIWORLD_AGENTCONTEXT_H
