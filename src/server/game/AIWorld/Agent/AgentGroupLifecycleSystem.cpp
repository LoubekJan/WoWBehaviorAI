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

void AgentGroupLifecycleSystem::RequestCreateGroup(AgentGroupKind kind, uint32 territoryMapId,
    float territoryX, float territoryY, float territoryZ, float resources, CoalitionFormationProfileId profileId,
    AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence, TransactionCallbackProcessor& pending,
    std::function<void(std::optional<GroupId>)> onComplete)
{
    std::optional<TransactionCallback> callback = persistence.CreateGroupAsync(kind, territoryMapId, territoryX, territoryY, territoryZ, resources, profileId,
        [&groupRegistry, kind, territoryMapId, territoryX, territoryY, territoryZ, resources, profileId, onComplete](bool success, GroupId newId)
        {
            if (!success)
            {
                onComplete(std::nullopt);
                return;
            }

            AgentGroupRecord record;
            record.Id = newId;
            record.Kind = kind;
            record.ProfileId = profileId;
            record.TerritoryMapId = territoryMapId;
            record.TerritoryX = territoryX;
            record.TerritoryY = territoryY;
            record.TerritoryZ = territoryZ;
            record.Resources = resources;
            record.Version = 0;

            if (!groupRegistry.Add(record))
            {
                // Should be unreachable - persistence just minted newId
                // fresh from its own counter, which only ever increases,
                // so a collision here would mean that counter and
                // groupRegistry have drifted out of sync with each other.
                // Defense in depth, not the primary guarantee - logged
                // loudly because it would indicate a real bug elsewhere.
                TC_LOG_ERROR("ai.world", "AgentGroupLifecycleSystem::RequestCreateGroup: freshly-created group id={} was already registered in groupRegistry - this should be unreachable",
                    newId.Value);
                onComplete(std::nullopt);
                return;
            }

            TC_LOG_INFO("ai.world", "AI agent group created id={} kind={} profile={} territoryMap={} resources={:.4f}",
                newId.Value, ToString(kind), ToString(profileId), territoryMapId, resources);

            onComplete(newId);
        });

    // std::nullopt means persistence already rejected this synchronously
    // and already invoked the wrapped callback above (with success=false)
    // - nothing left to enqueue.
    if (callback)
        pending.AddCallback(std::move(*callback));
}

void AgentGroupLifecycleSystem::RequestJoinGroup(GroupId groupId, AgentId memberId, uint64 joinedAtMs,
    AgentGroupRegistry& groupRegistry, AgentRegistry const& agentRegistry, AgentGroupPersistence& persistence,
    TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete)
{
    AgentGroupRecord* group = groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupLifecycleSystem::RequestJoinGroup: group id={} does not exist, refusing to join member id={}",
            groupId.Value, memberId.Value);
        onComplete(false);
        return;
    }

    // 2.12E2 P2 fix (STATIC review): rejects synchronously, before ever
    // touching the DB, if a Join/Leave/Dissolve is already in flight for
    // this GroupId - see this class's own header comment for the
    // DissolveGroup-then-JoinGroup orphan-membership race this closes.
    if (_pendingGroupOperations.contains(groupId.Value))
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestJoinGroup: group id={} already has a lifecycle operation in flight, refusing to join member id={}",
            groupId.Value, memberId.Value);
        onComplete(false);
        return;
    }

    // Read-only - RequestJoinGroup() never mutates an individual
    // AgentRecord, only confirms one already exists.
    if (!agentRegistry.Find(memberId))
    {
        TC_LOG_ERROR("ai.world", "AgentGroupLifecycleSystem::RequestJoinGroup: agent id={} does not exist, refusing to add it to group id={}",
            memberId.Value, groupId.Value);
        onComplete(false);
        return;
    }

    bool alreadyMember = std::any_of(group->Members.begin(), group->Members.end(),
        [memberId](AgentGroupMembership const& membership) { return membership.Member == memberId; });

    if (alreadyMember)
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestJoinGroup: agent id={} is already a member of group id={}, ignoring duplicate join",
            memberId.Value, groupId.Value);
        onComplete(false);
        return;
    }

    _pendingGroupOperations.insert(groupId.Value);

    TransactionCallback callback = persistence.AddGroupMemberAsync(groupId, memberId, joinedAtMs,
        [this, &groupRegistry, groupId, memberId, joinedAtMs, onComplete](bool success)
        {
            _pendingGroupOperations.erase(groupId.Value);

            if (!success)
            {
                onComplete(false);
                return;
            }

            // Re-resolved here, not the pointer captured above - see this
            // class's own header comment on why a completion never trusts
            // request-time validity. The pending-operation guard above
            // means the group cannot have been dissolved by another
            // Request* call while this join was in flight, but this is
            // kept as defense in depth regardless.
            AgentGroupRecord* group = groupRegistry.Find(groupId);
            if (!group)
            {
                TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestJoinGroup: group id={} no longer exists by the time the async join for member id={} completed",
                    groupId.Value, memberId.Value);
                onComplete(false);
                return;
            }

            AgentGroupMembership membership;
            membership.Member = memberId;
            membership.JoinedAtMs = joinedAtMs;
            group->Members.push_back(membership);

            TC_LOG_INFO("ai.world", "AI agent group join group={} member={} joinedAtMs={}", groupId.Value, memberId.Value, joinedAtMs);
            onComplete(true);
        });

    pending.AddCallback(std::move(callback));
}

void AgentGroupLifecycleSystem::RequestLeaveGroup(GroupId groupId, AgentId memberId,
    AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence,
    TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete)
{
    AgentGroupRecord* group = groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestLeaveGroup: group id={} does not exist, nothing to do for member id={}",
            groupId.Value, memberId.Value);
        onComplete(false);
        return;
    }

    // 2.12E2 P2 fix (STATIC review): see RequestJoinGroup()'s own comment.
    if (_pendingGroupOperations.contains(groupId.Value))
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestLeaveGroup: group id={} already has a lifecycle operation in flight, nothing to do for member id={}",
            groupId.Value, memberId.Value);
        onComplete(false);
        return;
    }

    auto it = std::find_if(group->Members.begin(), group->Members.end(),
        [memberId](AgentGroupMembership const& membership) { return membership.Member == memberId; });

    if (it == group->Members.end())
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestLeaveGroup: agent id={} is not a member of group id={}, nothing to do",
            memberId.Value, groupId.Value);
        onComplete(false);
        return;
    }

    _pendingGroupOperations.insert(groupId.Value);

    TransactionCallback callback = persistence.RemoveGroupMemberAsync(groupId, memberId,
        [this, &groupRegistry, groupId, memberId, onComplete](bool success)
        {
            _pendingGroupOperations.erase(groupId.Value);

            if (!success)
            {
                onComplete(false);
                return;
            }

            // Re-resolved here, not the iterator captured above - the
            // pending-operation guard above means the group cannot have
            // been dissolved by another Request* call while this leave was
            // in flight, but this is kept as defense in depth regardless,
            // and even a still-live AgentGroupRecord::Members has moved on
            // since the earlier find_if() ran.
            AgentGroupRecord* group = groupRegistry.Find(groupId);
            if (!group)
            {
                // The DB-side post-condition ("not a member") already
                // holds - the whole group, membership included, is gone -
                // so this is still a success, just nothing left to erase.
                TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestLeaveGroup: group id={} no longer exists by the time the async leave for member id={} completed",
                    groupId.Value, memberId.Value);
                onComplete(true);
                return;
            }

            auto it2 = std::find_if(group->Members.begin(), group->Members.end(),
                [memberId](AgentGroupMembership const& membership) { return membership.Member == memberId; });

            if (it2 != group->Members.end())
                group->Members.erase(it2);

            TC_LOG_INFO("ai.world", "AI agent group leave group={} member={}", groupId.Value, memberId.Value);
            onComplete(true);
        });

    pending.AddCallback(std::move(callback));
}

void AgentGroupLifecycleSystem::RequestDissolveGroup(GroupId groupId,
    AgentGroupRegistry& groupRegistry, AgentGroupPersistence& persistence,
    TransactionCallbackProcessor& pending, std::function<void(bool)> onComplete)
{
    AgentGroupRecord* group = groupRegistry.Find(groupId);
    if (!group)
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestDissolveGroup: group id={} does not exist, nothing to do", groupId.Value);
        onComplete(false);
        return;
    }

    // 2.12E2 P2 fix (STATIC review): see RequestJoinGroup()'s own comment.
    // This is the specific check that closes the original race the review
    // found: a Join/Leave submitted for a group whose dissolve is already
    // in flight is now rejected synchronously by that side's own check
    // above, instead of racing the dissolve's DELETE to commit second.
    if (_pendingGroupOperations.contains(groupId.Value))
    {
        TC_LOG_WARN("ai.world", "AgentGroupLifecycleSystem::RequestDissolveGroup: group id={} already has a lifecycle operation in flight, nothing to do", groupId.Value);
        onComplete(false);
        return;
    }

    uint32 formerMemberCount = uint32(group->Members.size());

    _pendingGroupOperations.insert(groupId.Value);

    TransactionCallback callback = persistence.DeleteGroupAsync(groupId,
        [this, &groupRegistry, groupId, formerMemberCount, onComplete](bool success)
        {
            _pendingGroupOperations.erase(groupId.Value);

            if (!success)
            {
                onComplete(false);
                return;
            }

            // Remove() is already safe/idempotent against groupId no
            // longer being present (returns false, harmless) - no
            // re-resolution needed the way Join/Leave's completions do.
            groupRegistry.Remove(groupId);

            TC_LOG_INFO("ai.world", "AI agent group dissolved group={} formerMemberCount={}", groupId.Value, formerMemberCount);
            onComplete(true);
        });

    pending.AddCallback(std::move(callback));
}
