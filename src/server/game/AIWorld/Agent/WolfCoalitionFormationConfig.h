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

#ifndef AIWORLD_WOLFCOALITIONFORMATIONCONFIG_H
#define AIWORLD_WOLFCOALITIONFORMATIONCONFIG_H

#include "Define.h"

// Milestone 2.12E4A: everything WolfCoalitionFormationSystem::Propose()
// needs beyond the candidate list itself - built fresh by AIWorldMgr for
// every RunWolfCoalitionFormation() call, the same "no state, every
// dependency a parameter" shape AgentGroupPolicyContext.h already
// documents. RadiusYards is AIWorld.WolfGroupFormationRadius's own value.
//
// MinMembers/MaxMembers are deliberately NOT independently-tunable values
// of their own - AIWorldMgr copies them straight from
// AgentGroupPolicyConfig::LooseMinMembers/LooseMaxMembers each call. A
// wolf pack IS a Loose AgentGroup, so its formation must obey the exact
// same size bounds AgentGroupPolicySystem::CanJoin()/ShouldDissolve()
// already enforce for one - a second, independently-configured min/max
// here could quietly drift out of sync with the policy layer's own rules
// and propose a group CanJoin() would then reject as GroupFull, or one
// ShouldDissolve() would flag as below minimum the moment it formed.
struct WolfCoalitionFormationConfig
{
    float RadiusYards = 30.0f;
    uint32 MinMembers = 2;
    uint32 MaxMembers = 5;
};

#endif // AIWORLD_WOLFCOALITIONFORMATIONCONFIG_H
