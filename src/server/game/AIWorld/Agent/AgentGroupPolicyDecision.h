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

#ifndef AIWORLD_AGENTGROUPPOLICYDECISION_H
#define AIWORLD_AGENTGROUPPOLICYDECISION_H

#include "Define.h"

// Milestone 2.12E3A: AgentGroupPolicySystem::CanJoin()/CanLeave()'s result -
// Allowed or a specific, loggable reason it is not. Deliberately an enum,
// not a bool, so a caller (and its logs) can say why a request was
// rejected without AgentGroupPolicySystem itself ever touching a logger,
// the DB, or AgentGroupRegistry - see AgentGroupPolicySystem.h's own class
// comment for why that boundary matters.
//
// GroupTooSmall and InvalidOperation are declared for API completeness
// but not yet returned by 2.12E3A's own deliberately simple rule set (see
// AgentGroupPolicySystem.cpp) - GroupTooSmall names a future CanLeave/
// CanJoin refinement (e.g. a minimum-size join requirement), and
// InvalidOperation names a future kind of malformed request neither
// CanJoin() nor CanLeave() currently has a case for. Reserved rather than
// invented on demand later, the same "declare the shape, wire the rule
// when it is actually needed" discipline AgentGroupKind.h's own
// Institutional-was-deliberately-not-added-yet comment already documents.
enum class AgentGroupPolicyDecision : uint8
{
    Allowed,
    GroupFull,
    GroupTooSmall,
    StableGroupProtected,
    InvalidMember,
    InvalidOperation
};

inline char const* ToString(AgentGroupPolicyDecision decision)
{
    switch (decision)
    {
        case AgentGroupPolicyDecision::Allowed:              return "ALLOWED";
        case AgentGroupPolicyDecision::GroupFull:            return "GROUP_FULL";
        case AgentGroupPolicyDecision::GroupTooSmall:        return "GROUP_TOO_SMALL";
        case AgentGroupPolicyDecision::StableGroupProtected: return "STABLE_GROUP_PROTECTED";
        case AgentGroupPolicyDecision::InvalidMember:        return "INVALID_MEMBER";
        case AgentGroupPolicyDecision::InvalidOperation:     return "INVALID_OPERATION";
        default:                                             return "UNKNOWN";
    }
}

#endif // AIWORLD_AGENTGROUPPOLICYDECISION_H
