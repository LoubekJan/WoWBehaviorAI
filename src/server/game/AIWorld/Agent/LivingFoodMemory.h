/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGFOODMEMORY_H
#define AIWORLD_LIVINGFOODMEMORY_H
#include "LivingReturnPolicy.h"

// Observations from this materialization only. A reached waypoint is a search,
// never a meal. Both positive and negative observations expire with respawns.
struct LivingFoodMemory
{
    struct Observation { ActionPosition Point; uint64 At; };
    std::vector<Observation> Meals, Searches, Blocked;
    uint32 EmptyRounds = 0, FailedAdvice = 0;
    static bool Recent(Observation const& o, uint64 now, uint64 age)
    { return now >= o.At && now - o.At < age; }
    static void Remember(std::vector<Observation>& list, ActionPosition const& point, uint64 now, size_t limit)
    {
        std::erase_if(list, [&](auto const& o) { return LivingReturnPolicy::Distance(o.Point, point) < 8; });
        if (list.size() >= limit) list.erase(list.begin());
        list.push_back({point, now});
    }
    void Searched(ActionPosition const& point, uint64 now)
    {
        Remember(Searches, point, now, 32);
        std::erase_if(Blocked, [&](auto const& o) { return LivingReturnPolicy::Distance(o.Point, point) < 8; });
    }
    void Unreachable(ActionPosition const& point, uint64 now) { Remember(Blocked, point, now, 32); }
    bool RecentlyBlocked(ActionPosition const& point, uint64 now) const
    {
        return std::any_of(Blocked.begin(), Blocked.end(), [&](auto const& o)
        { return Recent(o, now, 60000) && o.Point.MapId == point.MapId && LivingReturnPolicy::Distance(o.Point, point) < 8; });
    }
    void Fed(ActionPosition const& point, uint64 now)
    {
        Remember(Meals, point, now, 8);
        std::erase_if(Blocked, [&](auto const& o) { return LivingReturnPolicy::Distance(o.Point, point) < 8; });
        std::erase_if(Searches, [&](auto const& o) { return LivingReturnPolicy::Distance(o.Point, point) < 8; });
        EmptyRounds = FailedAdvice = 0;
    }
    uint32 Visits(ActionPosition const& point, uint64 now) const
    {
        return uint32(std::count_if(Searches.begin(), Searches.end(), [&](auto const& o)
            { return Recent(o, now, 600000) && LivingReturnPolicy::Distance(o.Point, point) < 8; }));
    }
    std::optional<ActionPosition> FoodHint(ActionPosition const& home, float radius, uint64 now) const
    {
        for (auto it = Meals.rbegin(); it != Meals.rend(); ++it)
            if (Recent(*it, now, 1800000) && it->Point.MapId == home.MapId &&
                std::hypot(it->Point.X-home.X, it->Point.Y-home.Y) <= radius &&
                !Visits(it->Point, now) && !RecentlyBlocked(it->Point, now)) return it->Point;
        return std::nullopt;
    }
    void EmptyRound() { EmptyRounds = std::min(EmptyRounds + 1, 2u); }
    uint64 AdviceFailed()
    {
        FailedAdvice = std::min(FailedAdvice + 1, 3u);
        return std::min<uint64>(900000, 120000ull << FailedAdvice);
    }
};
#endif
