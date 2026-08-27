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

#ifndef AIWORLD_COALITIONFORMATIONPROFILE_H
#define AIWORLD_COALITIONFORMATIONPROFILE_H

#include "AgentGroupKind.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"

// Milestone 2.12E4R (generalized from 2.12E4A's WolfCoalitionFormationConfig):
// everything CoalitionFormationSystem::Propose() needs beyond the candidate
// list itself, PLUS which kind of coalition this is (unlike the wolf-only
// predecessor this replaces, CoalitionFormationSystem itself no longer
// knows anything about wolves specifically - only about profiles). Built
// once by AIWorldMgr at Initialize() for each formation feature it
// supports - today exactly one, AIWorld's own WolfLoose profile
// (CreatureEntry = AIWorld.WolfGroupCreatureEntry, Kind = Loose,
// FormationRadius = AIWorld.WolfGroupFormationRadius, MinMembers/
// MaxMembers copied from AgentGroupPolicyConfig::LooseMinMembers/
// LooseMaxMembers - see this struct's own field comment for why those are
// never independently-tunable here). A second profile (e.g. a future
// bandit or caravan formation) is just another CoalitionFormationProfile
// value and a matching AIWorldMgr::RunCoalitionFormation() call site -
// CoalitionFormationSystem/CoalitionCandidate/CoalitionProposal need no
// change at all.
struct CoalitionFormationProfile
{
    CoalitionFormationProfileId Id = CoalitionFormationProfileId::WolfLoose;

    AgentGroupKind Kind = AgentGroupKind::Loose;

    // The one CoalitionCandidate::CreatureEntry this profile accepts -
    // CoalitionFormationSystem::Propose() filters candidates against this
    // before anything else (see its own class comment). A single fixed
    // entry, not a list/category - deliberately narrow, matching this
    // milestone's own one-profile scope.
    uint32 CreatureEntry = 0;

    // Deliberately NOT independently-tunable values of their own for the
    // WolfLoose profile - AIWorldMgr copies them straight from
    // AgentGroupPolicyConfig::LooseMinMembers/LooseMaxMembers when it
    // builds that profile. A wolf pack IS a Loose AgentGroup, so its
    // formation must obey the exact same size bounds
    // AgentGroupPolicySystem::CanJoin()/ShouldDissolve() already enforce
    // for one - a second, independently-configured min/max could quietly
    // drift out of sync with the policy layer's own rules and propose a
    // group CanJoin() would then reject as GroupFull, or one
    // ShouldDissolve() would flag as below minimum the moment it formed. A
    // future non-Loose profile is not bound by this precedent - it would
    // read whatever policy config actually governs its own Kind.
    uint32 MinMembers = 2;
    uint32 MaxMembers = 5;

    float FormationRadius = 30.0f;
};

#endif // AIWORLD_COALITIONFORMATIONPROFILE_H
