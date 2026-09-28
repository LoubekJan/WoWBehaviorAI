/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGESCAPEPROGRESS_H
#define AIWORLD_LIVINGESCAPEPROGRESS_H
#include "LivingReturnPolicy.h"

// Survives restarting the fallback flee generator. Only actual displacement
// or a changed threat resets progress, never a new action/animation.
struct LivingEscapeProgress
{
    ActionPosition Anchor, Danger;
    uint64 ProgressAt = 0, RetryAt = 0;
    uint32 Failures = 0;
    LivingReturnPolicy::RouteMemory Routes;
    bool Observe(uint64 now, ActionPosition const& here, ActionPosition const& danger)
    {
        if (!ProgressAt || now < ProgressAt || here.MapId != Anchor.MapId ||
            LivingReturnPolicy::Distance(here, Anchor) > 1.0f || LivingReturnPolicy::Distance(danger, Danger) > 2.0f)
        { Anchor = here; Danger = danger; ProgressAt = now; RetryAt = 0; Failures = 0; }
        return now >= ProgressAt && now - ProgressAt >= 8000;
    }
    void Failed(uint64 now)
    { Failures = std::min(5u, Failures + 1); RetryAt = now + Failures * 2000; }
};
#endif
