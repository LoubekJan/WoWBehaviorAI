/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_TELEMETRYPOPULATION_H
#define AIWORLD_TELEMETRYPOPULATION_H

#include "AgentTelemetrySnapshot.h"
#include <algorithm>
#include <unordered_set>

namespace AIWorldTelemetry
{
    // Native observations complete the configured population after managed
    // capture. Only successfully captured records claim a spawn: a missing
    // registry binding can still be shown as a native observation.
    inline std::vector<uint64> NativeSpawnIds(std::vector<uint64> const& configured,
        std::unordered_set<uint64> const& selected, std::vector<AgentTelemetrySnapshot> const& captured)
    {
        std::unordered_set<uint64> included;
        included.reserve(captured.size() + configured.size());
        for (auto const& item : captured)
            included.insert(item.SpawnId);
        std::vector<uint64> result;
        result.reserve(configured.size());
        for (uint64 spawn : configured)
            if (spawn && selected.count(spawn) && included.insert(spawn).second)
                result.push_back(spawn);
        return result;
    }

    inline bool HasManagedMemoryOwner(std::vector<AgentTelemetrySnapshot> const& captured, AgentId requested)
    {
        return requested && std::any_of(captured.begin(), captured.end(), [&](auto const& item)
        {
            return item.Agent == requested && !item.NativeCreature &&
                item.ControlMode == AgentControlMode::AIWorldControlled;
        });
    }
}

#endif
