/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGFORAGESTATE_H
#define AIWORLD_LIVINGFORAGESTATE_H
#include "NavigationDiagnostics.h"

// Per materialization. Scans and route attempts have separate clocks so an
// old observation of prey is never presented as a current reachability test.
struct LivingForageState
{
    uint64 ScannedAtMs = 0, SearchAtMs = 0;
    uint32 NearbyPrey = 0, AttackablePrey = 0, ReachablePrey = 0;
    uint32 RouteAttempts = 0, HeightRejected = 0, PathRejected = 0, StepsStarted = 0;
    NavigationDiagnostics Navigation;
};
#endif
