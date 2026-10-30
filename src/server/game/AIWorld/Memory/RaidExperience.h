/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_RAIDEXPERIENCE_H
#define AIWORLD_RAIDEXPERIENCE_H

#include "Define.h"

// Self-observed, session-only facts. Location and Target in the surrounding
// observation identify the actual encounter, not the intended remote depot.
enum class RaidExperienceKind : uint8
{
    StockObserved, GuardEncounter, RouteBlocked, SourceEmpty, Theft, Delivered,
    // A fresh local survey of one old guard position, not global knowledge
    // of that guard's whereabouts or a declaration that an area is safe.
    GuardPositionChecked
};

struct RaidExperience
{
    RaidExperienceKind Kind = RaidExperienceKind::StockObserved;
    uint32 DepotId = 0, RouteId = 0, Quantity = 0;
    uint64 AttemptId = 0;
};

inline bool SameRaidExperience(RaidExperience const& a, RaidExperience const& b)
{
    // Outcomes belong to individual attempts; repeated observations of one
    // event coalesce without turning one result into several successes.
    return a.Kind == b.Kind && a.DepotId == b.DepotId && a.RouteId == b.RouteId &&
        ((a.Kind != RaidExperienceKind::Theft && a.Kind != RaidExperienceKind::Delivered) ||
            a.AttemptId == b.AttemptId);
}

#endif
