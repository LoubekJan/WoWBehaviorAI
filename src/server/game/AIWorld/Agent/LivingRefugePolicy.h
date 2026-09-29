/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGREFUGEPOLICY_H
#define AIWORLD_LIVINGREFUGEPOLICY_H
#include "LivingReturnPolicy.h"

// Temporary local care, never a replacement spawn/home. An episode keeps its
// first anchor and original home bound even if combat interrupts returning.
struct LivingRefugeState
{
    static constexpr uint64 StalledMs = 300000, DurationMs = 120000, RetryMs = 60000;
    static constexpr float Radius = 12.0f;
    std::optional<ActionPosition> Anchor;
    ActionPosition Home;
    float HomeLimit = 0;
    uint64 Until = 0, NextAt = 0;
    uint32 Episodes = 0, Moves = 0, Blocked = 0;

    bool Active(uint64 now) const { return Anchor && now < Until; }
    bool Contains(ActionPosition const& point) const
    {
        return Anchor && LivingReturnPolicy::Finite(point) && point.MapId == Home.MapId &&
            LivingReturnPolicy::Distance(point, *Anchor) <= Radius &&
            std::hypot(point.X-Home.X, point.Y-Home.Y) <= HomeLimit;
    }
    bool Begin(uint64 now, uint64 returnStarted, uint64 progressAt, ActionPosition const& here,
        ActionPosition const& home, float limit)
    {
        if (Active(now) || now < NextAt || !returnStarted || now < returnStarted ||
            now-returnStarted < StalledMs || now < progressAt || now-progressAt < 60000 ||
            !LivingReturnPolicy::Finite(here) || !LivingReturnPolicy::Finite(home) ||
            here.MapId != home.MapId || !std::isfinite(limit) || limit <= 0) return false;
        if (!Anchor)
        {
            if (std::hypot(here.X-home.X, here.Y-home.Y) > limit) return false;
            Anchor = here; Home = home; HomeLimit = limit;
        }
        if (!Contains(here)) return false;
        Until = now + DurationMs; NextAt = Until + RetryMs;
        ++Episodes;
        return true;
    }
};
#endif
