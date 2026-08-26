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

#ifndef AIWORLD_AGENTGROUPOPERATIONSOURCE_H
#define AIWORLD_AGENTGROUPOPERATIONSOURCE_H

#include "Define.h"

// Milestone 2.12E3A: who is asking a group lifecycle operation to happen -
// the one piece of context AgentGroupPolicySystem needs to tell Loose and
// Stable apart in practice. Manual is a deliberately-authorized individual
// request (an admin/test command today; still a real, single-request
// action even once one exists for policy/AI-driven callers - see
// AutomaticPolicy below for the distinction that actually matters).
// AutomaticPolicy is a request some future automatic social-simulation
// pass generates on its own initiative (2.12E4's "nearby wolves form a
// pack" formation logic and its own dissolution/leave-drift decisions) -
// never issued in 2.12E3 itself, which adds no automatic caller of any
// kind, only the policy rules an eventual one will have to satisfy.
//
// This is NOT "Stable == immutable" - Stable groups can still change
// membership via Manual operations (see AgentGroupPolicySystem.h for the
// exact rules); AutomaticPolicy is only ever restricted for Stable, never
// disabled outright for it either (AgentGroupPolicySystem::CanJoin() does
// not check Source at all - a Stable group can still automatically gain a
// member up to capacity, it just cannot be automatically thinned out or
// dissolved once formed).
enum class AgentGroupOperationSource : uint8
{
    Manual,
    AutomaticPolicy
};

inline char const* ToString(AgentGroupOperationSource source)
{
    switch (source)
    {
        case AgentGroupOperationSource::Manual:          return "MANUAL";
        case AgentGroupOperationSource::AutomaticPolicy: return "AUTOMATIC_POLICY";
        default:                                         return "UNKNOWN";
    }
}

#endif // AIWORLD_AGENTGROUPOPERATIONSOURCE_H
