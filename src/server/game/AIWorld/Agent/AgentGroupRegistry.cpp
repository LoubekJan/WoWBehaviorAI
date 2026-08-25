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
    _groups.emplace(idValue, std::move(record));
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

std::vector<GroupId> AgentGroupRegistry::GetGroups() const
{
    std::vector<GroupId> ids;
    ids.reserve(_groups.size());
    for (auto const& entry : _groups)
        ids.push_back(entry.second.Id);
    return ids;
}
