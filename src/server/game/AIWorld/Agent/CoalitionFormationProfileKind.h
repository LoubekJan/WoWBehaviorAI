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

#ifndef AIWORLD_COALITIONFORMATIONPROFILEKIND_H
#define AIWORLD_COALITIONFORMATIONPROFILEKIND_H

#include "AgentGroupKind.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"
#include <optional>

// Milestone 2.12E4C2 P3 hardening (STATIC review): the one static mapping
// from a CoalitionFormationProfileId to the AgentGroupKind it is only ever
// valid to pair with - e.g. WolfLoose only ever governs a Loose
// AgentGroupRecord, never a Stable one. Returns std::nullopt for Invalid
// (no real profile, so no expected Kind to check against) and for any
// value CoalitionFormationProfileId does not actually define (a stray cast
// from an out-of-range integer, e.g. a bad config value).
//
// Deliberately a single, centrally-maintained helper rather than an
// "if (profileId == WolfLoose && kind != Loose)" check re-derived at every
// place that mutates AgentGroupRecord::ProfileId
// (AIWorldMgr::RunGroupProfileAdoption(), AgentGroupLifecycleSystem::
// RequestCreateGroup()) - a future second profile only ever needs a new
// case added here, not a matching check hand-copied into every caller.
inline std::optional<AgentGroupKind> GetCoalitionProfileKind(CoalitionFormationProfileId id)
{
    switch (id)
    {
        case CoalitionFormationProfileId::WolfLoose:
        case CoalitionFormationProfileId::DefiasLoose:
            return AgentGroupKind::Loose;

        case CoalitionFormationProfileId::Invalid:
        default:
            return std::nullopt;
    }
}

#endif // AIWORLD_COALITIONFORMATIONPROFILEKIND_H
