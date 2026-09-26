/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef TRINITY_NAVIGATIONDIAGNOSTICS_H
#define TRINITY_NAVIGATIONDIAGNOSTICS_H

#include "Define.h"
#include <optional>
#include <string>

// Value-only observations of the last query, not polygon references whose
// lifetime depends on loaded map tiles. Missing distances serialize as null.
struct NavigationDiagnostics
{
    bool Mesh = false, StartTile = false, EndTile = false;
    uint32 Filter = 0, StartFlags = 0, EndFlags = 0;
    std::optional<float> StartDistance, EndDistance;
    bool Swimming = false, Rejoin = false;
    std::string Failure = "NONE";
    std::string Detail = "NONE";
    float HomeRadius = 0.0f;
    std::optional<float> RejectedX, RejectedY, RejectedZ;
};
#endif
