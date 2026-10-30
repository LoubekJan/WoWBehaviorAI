/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGSUPPLYTELEMETRY_H
#define AIWORLD_LIVINGSUPPLYTELEMETRY_H

#include "Action/ActionPosition.h"
#include <optional>
#include <string>
#include <vector>

// Value-only runtime observations. Depot copies in the same batch come from
// the world-thread ledger, never asynchronous queries or persistent stocks.
struct FoodDepotTelemetry
{
    uint32 Id = 0;
    std::string Name;
    ActionPosition Position;
    uint32 Stock = 0, Capacity = 0, LowThreshold = 0;
    uint64 Delivered = 0, Consumed = 0, Stolen = 0, Exported = 0;
};

struct FoodAccountingTelemetry
{
    uint64 InitialFood = 0, Harvested = 0, InDepots = 0, InCargo = 0,
        Consumed = 0, Lost = 0;
    bool Conserved = false;
};

// Read-only coordination for the two authored caravan pairs. MemberReady is
// this actor's own physical arrival, while route and trip belong to the pair.
struct LivingCaravanTelemetry
{
    uint32 Id = 0;
    uint64 LeaderSpawnId = 0;
    std::vector<uint64> MemberSpawnIds;
    uint32 SourceDepotId = 0, DestinationDepotId = 7;
    std::string Stage;
    uint64 Trip = 0;
    uint32 Waypoint = 0;
    bool MemberReady = false;
    uint64 Loaded = 0, CompletedTrips = 0;
    std::optional<FoodDepotTelemetry> SourceDepot, DestinationDepot;
};

struct LivingSupplyTelemetry
{
    bool Enabled = false;
    std::string Role, Phase, LastResult;
    uint32 DepotId = 0, Cargo = 0, Capacity = 0;
    std::optional<uint32> CarcassRemaining;
    std::optional<uint64> CarcassSpawnId, CarcassLife;
    std::optional<ActionPosition> HuntingGround;
    std::optional<uint32> FieldId, FieldWorkpointIndex, FieldRemaining, FieldCapacity;
    std::optional<ActionPosition> FieldWorkpoint;
    std::optional<FoodDepotTelemetry> Depot;
    std::optional<FoodAccountingTelemetry> Accounting;
    std::optional<LivingCaravanTelemetry> Caravan;
    uint64 Harvested = 0, Delivered = 0, Consumed = 0, Lost = 0, Stolen = 0;
};

#endif
