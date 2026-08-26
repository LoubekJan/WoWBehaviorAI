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

#include "AgentGroupPolicySystem.h"
#include "AgentGroupMembership.h"
#include "AgentGroupRecord.h"
#include <algorithm>

namespace
{
    bool IsMember(AgentGroupRecord const& group, AgentId member)
    {
        return std::any_of(group.Members.begin(), group.Members.end(),
            [member](AgentGroupMembership const& membership) { return membership.Member == member; });
    }
}

AgentGroupPolicyDecision AgentGroupPolicySystem::CanJoin(AgentGroupRecord const& group, AgentId member, AgentGroupPolicyContext const& context) const
{
    if (!member || IsMember(group, member))
        return AgentGroupPolicyDecision::InvalidMember;

    uint32 maxMembers = group.Kind == AgentGroupKind::Loose ? context.Config.LooseMaxMembers : context.Config.StableMaxMembers;
    if (uint32(group.Members.size()) >= maxMembers)
        return AgentGroupPolicyDecision::GroupFull;

    return AgentGroupPolicyDecision::Allowed;
}

AgentGroupPolicyDecision AgentGroupPolicySystem::CanLeave(AgentGroupRecord const& group, AgentId member, AgentGroupPolicyContext const& context) const
{
    if (!member || !IsMember(group, member))
        return AgentGroupPolicyDecision::InvalidMember;

    if (context.Source == AgentGroupOperationSource::Manual)
        return AgentGroupPolicyDecision::Allowed;

    // AutomaticPolicy - the one rule that actually gives "Stable" a
    // meaning beyond bookkeeping, see this method's own header comment.
    if (group.Kind == AgentGroupKind::Stable)
        return AgentGroupPolicyDecision::StableGroupProtected;

    return AgentGroupPolicyDecision::Allowed;
}

bool AgentGroupPolicySystem::ShouldDissolve(AgentGroupRecord const& group, AgentGroupPolicyContext const& context) const
{
    if (group.Kind == AgentGroupKind::Stable)
        return false;

    return uint32(group.Members.size()) < context.Config.LooseMinMembers;
}
