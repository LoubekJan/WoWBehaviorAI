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

#ifndef AIWORLD_COALITIONMAINTENANCEDECISIONTYPE_H
#define AIWORLD_COALITIONMAINTENANCEDECISIONTYPE_H

#include "Define.h"

// Milestone 2.12E4C1: CoalitionMaintenanceSystem::Evaluate()'s result -
// what, if anything, an existing AgentGroup's current membership/spatial
// state suggests should happen next. Never itself an authorization -
// AIWorldMgr (2.12E4C2) is the one that turns a LeaveMember/DissolveGroup
// decision into an actual RequestLeaveGroupWithPolicy()/
// RequestDissolveGroupWithPolicy() call, both of which are already
// policy-gated (AgentGroupOperationSource::AutomaticPolicy) - see
// CoalitionMaintenanceSystem.h's own class comment for why Evaluate()
// itself deliberately never special-cases AgentGroupKind::Stable, and
// leaves that gate entirely to AgentGroupPolicySystem instead.
enum class CoalitionMaintenanceDecisionType : uint8
{
    None,
    LeaveMember,
    DissolveGroup
};

inline char const* ToString(CoalitionMaintenanceDecisionType type)
{
    switch (type)
    {
        case CoalitionMaintenanceDecisionType::None:          return "NONE";
        case CoalitionMaintenanceDecisionType::LeaveMember:   return "LEAVE_MEMBER";
        case CoalitionMaintenanceDecisionType::DissolveGroup: return "DISSOLVE_GROUP";
        default:                                              return "UNKNOWN";
    }
}

#endif // AIWORLD_COALITIONMAINTENANCEDECISIONTYPE_H
