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

#include "DynamicQuestRegistry.h"
#include "Log.h"

#include <algorithm>
#include <utility>

bool DynamicQuestRegistry::Add(DynamicQuestInstance instance)
{
    if (!instance.Id)
    {
        TC_LOG_ERROR("ai.world", "DynamicQuestRegistry::Add: refusing to add a quest with DynamicQuestId=0");
        return false;
    }

    if (Find(instance.Id))
    {
        TC_LOG_ERROR("ai.world", "DynamicQuestRegistry::Add: dynamic quest id={} is already registered, ignoring duplicate", instance.Id.Value);
        return false;
    }

    uint64 idValue = instance.Id.Value;
    _quests.emplace(idValue, std::move(instance));
    return true;
}

DynamicQuestInstance const* DynamicQuestRegistry::Find(DynamicQuestId id) const
{
    auto it = _quests.find(id.Value);
    return it != _quests.end() ? &it->second : nullptr;
}

bool DynamicQuestRegistry::ApplyTransition(DynamicQuestTransitionResult const& result)
{
    if (!result.IsAccepted())
        return false;

    DynamicQuestInstance const& next = *result.Instance;
    if (!next.Id)
        return false;

    auto it = _quests.find(next.Id.Value);
    if (it == _quests.end())
        return false;

    // Milestone 2.13C2 P2 fix, round 2 (STATIC review): optimistic
    // concurrency - result must have been computed from the CURRENTLY
    // stored revision, not some earlier one a second concurrently-
    // computed result also happened to start from. See this method's
    // own declaration comment for the concrete stale-overwrite scenario
    // this closes.
    if (it->second.Revision != result.SourceRevision)
    {
        TC_LOG_DEBUG("ai.world", "DynamicQuestRegistry::ApplyTransition: stale commit rejected for dynamic quest id={} (stored revision={}, result computed from revision={})",
            next.Id.Value, it->second.Revision, result.SourceRevision);
        return false;
    }

    it->second = next;
    return true;
}

bool DynamicQuestRegistry::Remove(DynamicQuestId id)
{
    return _quests.erase(id.Value) > 0;
}

uint32 DynamicQuestRegistry::GetCount() const
{
    return uint32(_quests.size());
}

DynamicQuestId DynamicQuestRegistry::GetHighestId() const
{
    if (_quests.empty())
        return DynamicQuestId{};

    return DynamicQuestId{_quests.rbegin()->first};
}

std::vector<DynamicQuestId> DynamicQuestRegistry::GetIdsAfterUntil(DynamicQuestId after, DynamicQuestId until, uint32 maxCount) const
{
    std::vector<DynamicQuestId> ids;
    if (maxCount == 0 || after.Value >= until.Value)
        return ids;

    ids.reserve(std::min<std::size_t>(maxCount, _quests.size()));

    auto it = _quests.upper_bound(after.Value);
    for (; it != _quests.end() && it->first <= until.Value && uint32(ids.size()) < maxCount; ++it)
        ids.push_back(DynamicQuestId{it->first});

    return ids;
}
