/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 * Licensed under the GNU General Public License, version 2 or later.
 */
#ifndef AIWORLD_MEMORYTELEMETRY_H
#define AIWORLD_MEMORYTELEMETRY_H

#include "Memory/LongTermMemoryRecord.h"
#include <string>
#include <string_view>
#include <vector>

inline constexpr uint32 TelemetryMemoryPageSize = 25;

// The only viewer request accepted by the exporter is a bounded memory read.
// The world thread independently checks that this agent belongs to its scope.
struct MemoryPageRequest
{
    std::string Id;
    AgentId Agent;
    uint32 Offset = 0;
    uint32 Anchor = 0;
};

struct MemoryTelemetryRecord
{
    LongTermMemoryRecord Memory;
    std::string ActorName, TargetName;
};

struct MemoryPageTelemetry
{
    MemoryPageRequest Request;
    uint64 Total = 0;
    uint64 Anchor = 0;
    std::vector<MemoryTelemetryRecord> Records;
};

TC_GAME_API std::optional<MemoryPageRequest> ParseMemoryPageRequest(std::string_view value);

#endif
