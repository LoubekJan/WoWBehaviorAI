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

#ifndef AIWORLD_COALITIONMAINTENANCEPROFILE_H
#define AIWORLD_COALITIONMAINTENANCEPROFILE_H

#include "AgentGroupKind.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"

// Milestone 2.12E4C1: everything CoalitionMaintenanceSystem::Evaluate()
// needs beyond the group/its members' own observations - built once by
// AIWorldMgr (2.12E4C2) alongside its matching CoalitionFormationProfile,
// the same "config struct built once, passed in per call" pattern
// CoalitionFormationProfile.h already documents. ProfileId identifies
// which formation profile's own hysteresis this pairs with (today:
// WolfLoose - see LeaveRadius's own comment for why the gap between it and
// CoalitionFormationProfile::FormationRadius matters) - reuses
// CoalitionFormationProfileId rather than inventing a second identity enum,
// since a maintenance profile only ever exists to police a group that some
// formation profile already knows how to grow.
//
// Kind/MinMembers deliberately do NOT gate any Stable-protection here - see
// CoalitionMaintenanceSystem.h's own class comment for why that stays
// AgentGroupPolicySystem's job alone, never duplicated in this struct or
// the system that consumes it.
struct CoalitionMaintenanceProfile
{
    CoalitionFormationProfileId ProfileId = CoalitionFormationProfileId::Invalid;

    AgentGroupKind Kind = AgentGroupKind::Loose;

    // Below this many CURRENT members, Evaluate() proposes DissolveGroup
    // outright, independent of any single member's own distance - the same
    // threshold AgentGroupPolicySystem::ShouldDissolve() already enforces
    // for Loose (see its own comment), duplicated here as a plain number
    // for the same reason CoalitionFormationProfile::MinMembers/MaxMembers
    // mirror AgentGroupPolicyConfig's own Loose bounds - not meant to be an
    // independently-tunable value of its own, always copied from the same
    // source of truth at the point AIWorldMgr builds this profile.
    uint32 MinMembers = 2;

    // Deliberately larger than the matching CoalitionFormationProfile's own
    // FormationRadius (today: 60 vs. 30) - the gap is what stops a member
    // drifting back and forth across a single boundary from repeatedly
    // forming, then immediately leaving, the same coalition every pass.
    // Below FormationRadius: eligible to form/join. Between
    // FormationRadius and LeaveRadius: already-formed membership holds, no
    // new formation either. Past LeaveRadius: Evaluate() may propose
    // LeaveMember.
    float LeaveRadius = 60.0f;
};

#endif // AIWORLD_COALITIONMAINTENANCEPROFILE_H
