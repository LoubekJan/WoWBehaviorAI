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

DynamicQuestInstance* DynamicQuestRegistry::Find(DynamicQuestId id)
{
    auto it = _quests.find(id.Value);
    return it != _quests.end() ? &it->second : nullptr;
}

DynamicQuestInstance const* DynamicQuestRegistry::Find(DynamicQuestId id) const
{
    auto it = _quests.find(id.Value);
    return it != _quests.end() ? &it->second : nullptr;
}

bool DynamicQuestRegistry::Remove(DynamicQuestId id)
{
    return _quests.erase(id.Value) > 0;
}

uint32 DynamicQuestRegistry::GetCount() const
{
    return uint32(_quests.size());
}
