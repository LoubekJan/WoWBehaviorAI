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

#ifndef AIWORLD_AGENTGROUPINTENTTYPE_H
#define AIWORLD_AGENTGROUPINTENTTYPE_H

#include "Define.h"

// Milestone 2.12F1: AgentGroupIntentSystem::Evaluate()'s result - what, if
// anything, a group currently wants AS A GROUP, before any of that intent
// is decomposed into individual member action proposals (that split is
// deliberately AgentGroupIntentProjector's own job, 2.12F2 - see
// AgentGroupIntent.h for why a group-level intent never itself names
// which member should do what). Never itself an authorization or a
// movement command - AgentGroupIntentSystem never touches AgentRecord/
// Creature/ActionSystem, it only ever answers "what does this group want".
//
// Deliberately generic: nothing here (or in AgentGroupIntentSystem/
// AgentGroupCoordinationProfile) knows what a wolf, a bandit, a guard
// patrol, or a caravan is - only WHICH profile a group's own persistent
// AgentGroupRecord::ProfileId resolves to decides that, entirely outside
// this enum's own concern.
//
// Milestone 2.12G2: Roam is the second real value - deliberately not just
// a renamed Regroup. Both are still "what does this group want, before
// any per-member decomposition" facts, but they answer different
// questions (Regroup: "is anyone too far from the group's own fixed
// territory point"; Roam: "does the group want to move ITS OWN territory-
// relative target right now") and AgentGroupIntentProjector/AIWorldMgr
// both give each its own GoalType/radius semantics rather than treating
// Roam as an alias - see AgentGroupIntentSystem.h/AgentGroupIntentProjector.h
// for the full split.
enum class AgentGroupIntentType : uint8
{
    None = 0,
    Regroup = 1,
    Roam = 2
};

inline char const* ToString(AgentGroupIntentType type)
{
    switch (type)
    {
        case AgentGroupIntentType::None:    return "NONE";
        case AgentGroupIntentType::Regroup: return "REGROUP";
        case AgentGroupIntentType::Roam:    return "ROAM";
        default:                            return "UNKNOWN";
    }
}

#endif // AIWORLD_AGENTGROUPINTENTTYPE_H
