/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRAIDTELEMETRY_H
#define AIWORLD_LIVINGRAIDTELEMETRY_H

#include "LivingSupplyTelemetry.h"
#include <vector>

struct LivingRaidPlanningTelemetry
{
    std::string Reason;
    uint32 MemoryRecords = 0, RouteId = 0;
    std::optional<uint32> KnownStock;
    std::optional<uint64> StockAgeMs;
    float Risk = 0;
    uint64 RetryRemainingMs = 0;
    std::optional<ActionPosition> Danger;
    uint64 DangerGuardSpawnId = 0, DangerAgeMs = 0;
    uint32 CheckedPositions = 0;
    std::optional<ActionPosition> LastCheckedPosition;
    uint64 LastCheckedGuardSpawnId = 0, LastCheckedAgeMs = 0;
};

struct LivingRaidPartyTelemetry
{
    uint64 Id = 0, LeaderSpawnId = 0;
    std::vector<uint64> MemberSpawnIds;
    std::string Phase, Role;
};

// One actor's current, world-thread raid observation. Attempt identity belongs
// to this runtime incarnation; event counts are distinct from food portions.
struct LivingRaidTelemetry
{
    bool Enabled = false;
    std::string Role, Phase, Outcome;
    uint64 AttemptId = 0, TargetSpawnId = 0;
    uint64 Noticed = 0, Interceptions = 0, Thefts = 0, Delivered = 0, Prevented = 0;
    uint32 SourceDepotId = 0, CampDepotId = 0;
    std::optional<FoodDepotTelemetry> SourceDepot, CampDepot;
    std::optional<LivingRaidPlanningTelemetry> Planning;
    std::optional<LivingRaidPartyTelemetry> Party;
};

#endif
