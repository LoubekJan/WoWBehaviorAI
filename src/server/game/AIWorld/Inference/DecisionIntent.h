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

#ifndef AIWORLD_DECISIONINTENT_H
#define AIWORLD_DECISIONINTENT_H

#include "DecisionIntentType.h"
#include "Define.h"

// Milestone 2.9B: what ai-server's deterministic policy proposes for one
// agent - Type only, deliberately no Destination/FleeFromGuid/other
// execution parameters yet. Those belong to a later milestone's
// DecisionIntent -> ActionRequest translation, and must be resolved from
// world-thread authoritative data (the actor's actual current threat
// victim, actual current goal, ...) the same "honesty, not trust" way
// ActionRequest.h's own SourceGoal/FleeFromGuid already are - never taken
// on faith from what ai-server claims. Until that translation exists,
// AIWorldMgr::Update() only logs this as an audit trail - see its 2.9B
// comment for why it deliberately never builds an ActionRequest from it.
struct DecisionIntent
{
    DecisionIntentType Type = DecisionIntentType::None;
};

#endif // AIWORLD_DECISIONINTENT_H
