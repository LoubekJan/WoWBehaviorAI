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

#ifndef AIWORLD_AGENTGROUPCOORDINATIONPROFILE_H
#define AIWORLD_AGENTGROUPCOORDINATIONPROFILE_H

#include "AgentGroupKind.h"
#include "CoalitionFormationProfileId.h"
#include "Define.h"

// Milestone 2.12F1: everything AgentGroupIntentSystem::Evaluate() needs
// beyond the group/its members' own observations - the coordination-side
// counterpart to CoalitionFormationProfile/CoalitionMaintenanceProfile,
// the same "config struct built once by AIWorldMgr, passed in per call"
// pattern, and the same reuse of CoalitionFormationProfileId as the one
// identity a group's own persistent provenance (AgentGroupRecord::
// ProfileId) is checked against - see AgentGroupIntentSystem.h for why
// Kind/ProfileId are both validated the same fail-closed way
// CoalitionMaintenanceProfile already is.
//
// RegroupEnabled/RegroupRadius are the one coordination behavior this
// milestone defines - a profile that does not want automatic regrouping
// at all simply leaves RegroupEnabled false, the same "declare the shape,
// wire the rule only when it is actually needed" discipline this
// codebase already holds elsewhere (see AgentGroupKind.h's own
// Institutional-not-yet-added comment). WolfLoose is the one real
// profile that sets this today; a future GuardPatrol/BanditGroup/
// Caravan/CivilianGroup profile is just another value with its own
// RegroupRadius (or RegroupEnabled = false if that Kind of group never
// automatically regroups at all) - AgentGroupIntentSystem itself never
// special-cases which profile this is, and never will just from adding
// a new one here.
struct AgentGroupCoordinationProfile
{
    CoalitionFormationProfileId ProfileId = CoalitionFormationProfileId::Invalid;
    AgentGroupKind Kind = AgentGroupKind::Loose;

    bool RegroupEnabled = false;
    float RegroupRadius = 0.0f;
};

#endif // AIWORLD_AGENTGROUPCOORDINATIONPROFILE_H
