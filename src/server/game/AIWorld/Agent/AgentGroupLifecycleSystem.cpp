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

#include "AgentGroupLifecycleSystem.h"
#include "AgentGroupMembership.h"
#include "AgentGroupRecord.h"
#include "AgentGroupRegistry.h"
#include "AgentRegistry.h"
#include "Log.h"
#include "Persistence/AgentGroupPersistence.h"
#include <algorithm>

std::optional<GroupId> AgentGroupLifecycleSystem::CreateGroup(AgentGroupKind kind, uint32 territoryMapId,
    float territoryX, float territoryY, float territoryZ, float resources,
    AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence) const
{
    GroupId groupId = persistence.CreateGroup(kind, territoryMapId, territoryX, territoryY, territoryZ, resources);
    if (!groupId)
    {
        // AgentGroupPersistence::CreateGroup() already logged why.
        return std::nullopt;
    }

    AgentGroupRecord record;
    record.Id = groupId;
    record.Kind = kind;
    record.TerritoryMapId = territoryMapId;
    record.TerritoryX = territoryX;
    record.TerritoryY = territoryY;
    record.TerritoryZ = territoryZ;
    record.Resources = resources;
    record.Version = 0;

    if (!groupRegistry.Add(record))
    {
        // Should be unreachable - persistence just minted groupId fresh
        // from its own counter, which only ever increases, so a collision
        // here would mean that counter and groupRegistry have drifted out
        // of sync with each other. Defense in depth, not the primary
        // guarantee - logged loudly because it would indicate a real bug
        // elsewhere, not an expected runtime condition.
        TC_LOG_ERROR("ai.world", "AgentGroupLifecycleSystem::CreateGroup: freshly-minted group id={} was already registered in groupRegistry - this should be unreachable",
            groupId.Value);
        return std::nullopt;
    }

    TC_LOG_INFO("ai.world", "AI agent group created id={} kind={} territoryMap={} resources={:.4f}",
        groupId.Value, ToString(kind), territoryMapId, resources);

    return groupId;
}

bool AgentGroupLifecycleSystem::JoinGroup(GroupId groupId, AgentId memberId, uint64 joinedAtMs,
    AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry, AgentGroupPersistence& persistence) const
{
    AgentGroupRecord* group = groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupLifecycleSystem::JoinGroup: group id={} does not exist, refusing to join member id={}",
            groupId.Value, memberId.Value);
        return false;
    }

    // Read-only - JoinGroup() never mutates an individual AgentRecord,
    // only confirms one already exists.
    if (!agentRegistry.Find(memberId))
    {
        TC_LOG_ERROR("ai.world", "AgentGroupLifecycleSystem::JoinGroup: agent id={} does not exist, refusing to add it to group id={}",
            memberId.Value, groupId.Value);
        return false;
    }

    bool alreadyMember = std::any_of(group->Members.begin(), group->Members.end(),
        [memberId](AgentGroupMembership const& membership) { return membership.Member == memberId; });

    if (alreadyMember)
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::JoinGroup: agent id={} is already a member of group id={}, ignoring duplicate join",
            memberId.Value, groupId.Value);
        return false;
    }

    persistence.AddGroupMember(groupId, memberId, joinedAtMs);

    AgentGroupMembership membership;
    membership.Member = memberId;
    membership.JoinedAtMs = joinedAtMs;
    group->Members.push_back(membership);

    TC_LOG_INFO("ai.world", "AI agent group join group={} member={} joinedAtMs={}", groupId.Value, memberId.Value, joinedAtMs);
    return true;
}

bool AgentGroupLifecycleSystem::LeaveGroup(GroupId groupId, AgentId memberId,
    AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence) const
{
    AgentGroupRecord* group = groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::LeaveGroup: group id={} does not exist, nothing to do for member id={}",
            groupId.Value, memberId.Value);
        return false;
    }

    auto it = std::find_if(group->Members.begin(), group->Members.end(),
        [memberId](AgentGroupMembership const& membership) { return membership.Member == memberId; });

    if (it == group->Members.end())
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::LeaveGroup: agent id={} is not a member of group id={}, nothing to do",
            memberId.Value, groupId.Value);
        return false;
    }

    persistence.RemoveGroupMember(groupId, memberId);
    group->Members.erase(it);

    TC_LOG_INFO("ai.world", "AI agent group leave group={} member={}", groupId.Value, memberId.Value);
    return true;
}

bool AgentGroupLifecycleSystem::DissolveGroup(GroupId groupId,
    AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence) const
{
    AgentGroupRecord* group = groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::DissolveGroup: group id={} does not exist, nothing to do", groupId.Value);
        return false;
    }

    uint32 formerMemberCount = uint32(group->Members.size());

    // Persistence first, then the runtime registry - same ordering
    // CreateGroup()/JoinGroup()/LeaveGroup() all use, so groupRegistry
    // never disagrees with what is actually in the DB for longer than it
    // takes this one call to run. Never touches AgentRegistry/AgentRecord/
    // Creature for any former member - see this class's own header
    // comment.
    persistence.DeleteGroup(groupId);
    groupRegistry.Remove(groupId);

    TC_LOG_INFO("ai.world", "AI agent group dissolved group={} formerMemberCount={}", groupId.Value, formerMemberCount);
    return true;
}
