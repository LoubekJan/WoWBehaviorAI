/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AIWORLD_COALITIONFORMATIONPROFILEID_H
#define AIWORLD_COALITIONFORMATIONPROFILEID_H

#include "Define.h"

// Milestone 2.12E4R: which CoalitionFormationProfile a formation
// pass/attempt belongs to - identity only, never itself carried across the
// generic CoalitionFormationSystem/CoalitionCandidate/CoalitionProposal
// pipeline (those stay entirely unaware that "WolfLoose" is the only
// profile that exists today). Used to key AIWorldMgr's own per-profile
// in-flight tracking (see AIWorldMgr::_formationInFlight) and to name a
// formation attempt in logs. WolfLoose is the only value that exists yet -
// see CoalitionFormationProfile.h for why adding a second profile (e.g. a
// future bandit or caravan formation) needs no change to
// CoalitionFormationSystem/AIWorldMgr's own orchestration methods, only a
// new CoalitionFormationProfile value and a new enumerator here.
enum class CoalitionFormationProfileId : uint8
{
    WolfLoose
};

inline char const* ToString(CoalitionFormationProfileId id)
{
    switch (id)
    {
        case CoalitionFormationProfileId::WolfLoose: return "WOLF_LOOSE";
        default:                                     return "UNKNOWN";
    }
}

#endif // AIWORLD_COALITIONFORMATIONPROFILEID_H
