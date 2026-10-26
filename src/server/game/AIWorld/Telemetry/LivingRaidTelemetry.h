/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRAIDTELEMETRY_H
#define AIWORLD_LIVINGRAIDTELEMETRY_H

#include "LivingSupplyTelemetry.h"

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
};

#endif
