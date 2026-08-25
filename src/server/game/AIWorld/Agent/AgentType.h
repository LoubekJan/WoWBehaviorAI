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

#ifndef AIWORLD_AGENTTYPE_H
#define AIWORLD_AGENTTYPE_H

#include "Define.h"

enum class AgentType : uint8
{
    Civilian = 0,
    Guard = 1,
    Merchant = 2,

    // Milestone 2.12C: renamed from CreatureGroup - a social layer over
    // independent member agents (each with its own AgentId/identity/
    // memory/needs/goal/decision/actions/Creature lifecycle - see
    // AgentGroupState.h/AgentGroupMembership.h), never an aggregate that
    // stands in for its members. Value 3 kept unchanged across the rename
    // - only ever a persisted uint8 (ai_agents.agent_type), not a wire
    // protocol value anything external parses.
    AgentGroup = 3
};

// Materialized: bound to a currently-loaded Creature (RuntimeGuid valid).
// Abstract: no live Creature right now (grid unloaded, not yet spawned,
// etc.) - the agent itself still exists in AgentRegistry either way.
enum class AgentWorldState : uint8
{
    Abstract = 0,
    Materialized = 1
};

inline char const* ToString(AgentType type)
{
    switch (type)
    {
        case AgentType::Civilian:   return "CIVILIAN";
        case AgentType::Guard:      return "GUARD";
        case AgentType::Merchant:   return "MERCHANT";
        case AgentType::AgentGroup: return "AGENT_GROUP";
        default:                    return "UNKNOWN";
    }
}

#endif // AIWORLD_AGENTTYPE_H
