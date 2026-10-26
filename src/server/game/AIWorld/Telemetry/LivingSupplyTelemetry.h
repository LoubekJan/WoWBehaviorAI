/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGSUPPLYTELEMETRY_H
#define AIWORLD_LIVINGSUPPLYTELEMETRY_H

#include "Action/ActionPosition.h"
#include <optional>
#include <string>

// Value-only runtime observations. Depot copies in the same batch come from
// the world-thread ledger, never asynchronous queries or persistent stocks.
struct FoodDepotTelemetry
{
    uint32 Id = 0;
    std::string Name;
    ActionPosition Position;
    uint32 Stock = 0, Capacity = 0, LowThreshold = 0;
    uint64 Delivered = 0, Consumed = 0;
};

struct FoodAccountingTelemetry
{
    uint64 InitialFood = 0, Harvested = 0, InDepots = 0, InCargo = 0,
        Consumed = 0, Lost = 0;
    bool Conserved = false;
};

struct LivingSupplyTelemetry
{
    bool Enabled = false;
    std::string Role, Phase, LastResult;
    uint32 DepotId = 0, Cargo = 0, Capacity = 0;
    std::optional<uint32> CarcassRemaining;
    std::optional<uint64> CarcassSpawnId, CarcassLife;
    std::optional<ActionPosition> HuntingGround;
    std::optional<FoodDepotTelemetry> Depot;
    std::optional<FoodAccountingTelemetry> Accounting;
    uint64 Harvested = 0, Delivered = 0, Consumed = 0, Lost = 0;
};

#endif
