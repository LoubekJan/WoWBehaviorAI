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
#include "Inference/QuestProposal.h"
#include "Log.h"

#include <algorithm>
#include <utility>

DynamicQuestTransitionResult DynamicQuestRegistry::Offer(DynamicQuestId id, QuestProposal const& proposal, uint64 nowMs)
{
    DynamicQuestTransitionResult result = OfferDynamicQuest(id, proposal, nowMs);
    if (!result.IsAccepted())
        return result;

    uint64 idValue = result.Instance->Id.Value;
    if (_quests.find(idValue) != _quests.end())
    {
        TC_LOG_ERROR("ai.world", "DynamicQuestRegistry::Offer: dynamic quest id={} is already registered, ignoring duplicate", idValue);
        DynamicQuestTransitionResult duplicate;
        duplicate.Reason = DynamicQuestRejectReason::DuplicateQuestId;
        return duplicate;
    }

    _quests.emplace(idValue, *result.Instance);
    return result;
}

DynamicQuestInstance const* DynamicQuestRegistry::Find(DynamicQuestId id) const
{
    auto it = _quests.find(id.Value);
    return it != _quests.end() ? &it->second : nullptr;
}

namespace
{
    DynamicQuestTransitionResult QuestNotFound()
    {
        DynamicQuestTransitionResult result;
        result.Reason = DynamicQuestRejectReason::QuestNotFound;
        return result;
    }
}

DynamicQuestTransitionResult DynamicQuestRegistry::Accept(DynamicQuestId id, ObjectGuid playerGuid, uint64 nowMs)
{
    auto it = _quests.find(id.Value);
    if (it == _quests.end())
        return QuestNotFound();

    DynamicQuestTransitionResult result = AcceptDynamicQuest(it->second, playerGuid, nowMs);
    if (result.IsAccepted())
        it->second = *result.Instance;

    return result;
}

DynamicQuestTransitionResult DynamicQuestRegistry::ApplyProgress(DynamicQuestId id, ObjectGuid playerGuid, uint64 progressEventId, uint64 nowMs)
{
    auto it = _quests.find(id.Value);
    if (it == _quests.end())
        return QuestNotFound();

    DynamicQuestTransitionResult result = ApplyDynamicQuestProgress(it->second, playerGuid, progressEventId, nowMs);
    if (result.IsAccepted())
        it->second = *result.Instance;

    return result;
}

DynamicQuestTransitionResult DynamicQuestRegistry::Complete(DynamicQuestId id, uint64 nowMs)
{
    auto it = _quests.find(id.Value);
    if (it == _quests.end())
        return QuestNotFound();

    DynamicQuestTransitionResult result = CompleteDynamicQuest(it->second, nowMs);
    if (result.IsAccepted())
        it->second = *result.Instance;

    return result;
}

DynamicQuestTransitionResult DynamicQuestRegistry::Fail(DynamicQuestId id, uint64 nowMs)
{
    auto it = _quests.find(id.Value);
    if (it == _quests.end())
        return QuestNotFound();

    DynamicQuestTransitionResult result = FailDynamicQuest(it->second, nowMs);
    if (result.IsAccepted())
        it->second = *result.Instance;

    return result;
}

DynamicQuestTransitionResult DynamicQuestRegistry::Expire(DynamicQuestId id, uint64 nowMs)
{
    auto it = _quests.find(id.Value);
    if (it == _quests.end())
        return QuestNotFound();

    DynamicQuestTransitionResult result = ExpireDynamicQuest(it->second, nowMs);
    if (result.IsAccepted())
        it->second = *result.Instance;

    return result;
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
