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

#include "AgentGroupRegistry.h"
#include "Log.h"
#include <algorithm>
#include <utility>

bool AgentGroupRegistry::Add(AgentGroupRecord record)
{
    if (!record.Id)
    {
        TC_LOG_ERROR("ai.world", "AgentGroupRegistry::Add: refusing to add a group with GroupId=0");
        return false;
    }

    if (Find(record.Id))
    {
        TC_LOG_ERROR("ai.world", "AgentGroupRegistry::Add: group id={} is already registered, ignoring duplicate", record.Id.Value);
        return false;
    }

    uint64 idValue = record.Id.Value;
    auto emplaced = _groups.emplace(idValue, std::move(record));

    // 2.12F2 P3 fix (STATIC review): indexes whatever Members the record
    // already carried, if any - see this method's own header comment.
    for (AgentGroupMembership const& membership : emplaced.first->second.Members)
        _memberGroups[membership.Member.Value].insert(idValue);

    return true;
}

AgentGroupRecord* AgentGroupRegistry::Find(GroupId id)
{
    auto it = _groups.find(id.Value);
    return it != _groups.end() ? &it->second : nullptr;
}

AgentGroupRecord const* AgentGroupRegistry::Find(GroupId id) const
{
    auto it = _groups.find(id.Value);
    return it != _groups.end() ? &it->second : nullptr;
}

bool AgentGroupRegistry::Remove(GroupId id)
{
    auto it = _groups.find(id.Value);
    if (it == _groups.end())
        return false;

    // 2.12F2 P3 fix (STATIC review): every one of this group's own members
    // is removed from _memberGroups too - see this method's own header
    // comment. Erases the per-member entry entirely once it is empty,
    // rather than leaving a stale empty set behind for an agent that no
    // longer belongs to anything.
    for (AgentGroupMembership const& membership : it->second.Members)
    {
        auto memberIt = _memberGroups.find(membership.Member.Value);
        if (memberIt == _memberGroups.end())
            continue;

        memberIt->second.erase(id.Value);
        if (memberIt->second.empty())
            _memberGroups.erase(memberIt);
    }

    _groups.erase(it);
    return true;
}

bool AgentGroupRegistry::AddMember(GroupId groupId, AgentGroupMembership const& membership)
{
    AgentGroupRecord* group = Find(groupId);
    if (!group)
        return false;

    // 2.12F2 P3 fix, round 2 (STATIC review): fail-closed against a
    // duplicate membership.Member, checked here rather than trusted to
    // every caller - _memberGroups indexes by AgentId into an
    // unordered_SET of GroupId (see its own declaration comment), so a
    // second AddMember(groupId, sameMember) would insert a second,
    // indistinguishable entry into the forward Members vector while the
    // reverse side silently stays a one-element set (insert() of an
    // already-present value is a no-op). RemoveMember() then erases only
    // the FIRST forward entry it finds but the WHOLE reverse entry - after
    // that, AgentGroupRecord::Members still (correctly) lists the member
    // once, while GetGroupsOfMember()/IsMemberOfKind() both (incorrectly)
    // report them as not a member of anything, the exact forward/reverse
    // divergence this whole index exists to prevent. AgentGroupLifecycleSystem::
    // RequestJoinGroup() already rejects a duplicate before ever reaching
    // here, but this is now the authoritative membership-mutation
    // boundary and must not depend on every caller re-deriving that check
    // correctly itself.
    bool alreadyMember = std::any_of(group->Members.begin(), group->Members.end(),
        [&membership](AgentGroupMembership const& existing) { return existing.Member == membership.Member; });
    if (alreadyMember)
        return false;

    group->Members.push_back(membership);
    _memberGroups[membership.Member.Value].insert(groupId.Value);
    return true;
}

bool AgentGroupRegistry::RemoveMember(GroupId groupId, AgentId member)
{
    AgentGroupRecord* group = Find(groupId);
    if (!group)
        return false;

    auto it = std::find_if(group->Members.begin(), group->Members.end(),
        [member](AgentGroupMembership const& membership) { return membership.Member == member; });
    if (it == group->Members.end())
        return false;

    group->Members.erase(it);

    auto memberIt = _memberGroups.find(member.Value);
    if (memberIt != _memberGroups.end())
    {
        memberIt->second.erase(groupId.Value);
        if (memberIt->second.empty())
            _memberGroups.erase(memberIt);
    }

    return true;
}

std::vector<GroupId> AgentGroupRegistry::GetGroupsOfMember(AgentId member) const
{
    std::vector<GroupId> groupIds;

    auto it = _memberGroups.find(member.Value);
    if (it == _memberGroups.end())
        return groupIds;

    groupIds.reserve(it->second.size());
    for (uint64 groupIdValue : it->second)
        groupIds.push_back(GroupId{ groupIdValue });

    return groupIds;
}

std::vector<GroupId> AgentGroupRegistry::GetGroups() const
{
    std::vector<GroupId> ids;
    ids.reserve(_groups.size());
    for (auto const& entry : _groups)
        ids.push_back(entry.second.Id);
    return ids;
}

GroupId AgentGroupRegistry::GetHighestGroupId() const
{
    if (_groups.empty())
        return GroupId{};

    return _groups.rbegin()->second.Id;
}

std::vector<GroupId> AgentGroupRegistry::GetGroupsAfterUntil(GroupId after, GroupId until, uint32 maxCount) const
{
    std::vector<GroupId> ids;
    if (maxCount == 0 || after.Value >= until.Value)
        return ids;

    ids.reserve(std::min<std::size_t>(maxCount, _groups.size()));

    auto it = _groups.upper_bound(after.Value);
    for (; it != _groups.end() && it->first <= until.Value && uint32(ids.size()) < maxCount; ++it)
        ids.push_back(it->second.Id);

    return ids;
}

bool AgentGroupRegistry::IsMemberOfKind(AgentId member, AgentGroupKind kind) const
{
    // 2.12F2 P3 fix (STATIC review): via _memberGroups now, not a linear
    // scan of every registered group - see this method's own header
    // comment.
    auto it = _memberGroups.find(member.Value);
    if (it == _memberGroups.end())
        return false;

    for (uint64 groupIdValue : it->second)
    {
        auto groupIt = _groups.find(groupIdValue);
        if (groupIt != _groups.end() && groupIt->second.Kind == kind)
            return true;
    }

    return false;
}
