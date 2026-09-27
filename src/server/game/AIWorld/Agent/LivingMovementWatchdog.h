/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGMOVEMENTWATCHDOG_H
#define AIWORLD_LIVINGMOVEMENTWATCHDOG_H
#include "LivingReturnPolicy.h"

struct LivingMovementWatchdog
{
    uint64 StartedAt = 0, ProgressAt = 0;
    ActionPosition Anchor;
    char const* End = "NONE";
    void Begin(uint64 now, ActionPosition const& here)
    { StartedAt = ProgressAt = now; Anchor = here; End = "MOVING"; }
    // Detours can initially move away from the destination. Measure real
    // displacement, with a separate ceiling so loops cannot last forever.
    bool Continue(uint64 now, ActionPosition const& here, bool escape)
    {
        if (LivingReturnPolicy::Distance(Anchor, here) > 1.0f)
        { Anchor = here; ProgressAt = now; }
        if (now < StartedAt || now - StartedAt >= (escape ? 8000u : 180000u))
        { End = escape ? "ESCAPE_LIMIT" : "DURATION_LIMIT"; return false; }
        if (now >= ProgressAt && now - ProgressAt >= 15000)
        { End = "NO_PROGRESS"; return false; }
        return true;
    }
};
#endif
