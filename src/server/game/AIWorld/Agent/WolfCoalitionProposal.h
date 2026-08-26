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

#ifndef AIWORLD_WOLFCOALITIONPROPOSAL_H
#define AIWORLD_WOLFCOALITIONPROPOSAL_H

#include "AgentId.h"
#include "Define.h"
#include <vector>

// Milestone 2.12E4A: WolfCoalitionFormationSystem::Propose()'s output - a
// deterministic, pure-value description of a Loose AgentGroup that COULD
// form, never one that already has. Members holds at least
// WolfCoalitionFormationConfig::MinMembers AgentIds whenever a proposal is
// returned at all - see Propose()'s own comment for why. TerritoryX/Y/Z/
// MapId is the proposal's seed candidate's own position at scan time - the
// same role AgentGroupRecord::TerritoryMapId/X/Y/Z already plays once a
// real AgentGroupRecord exists, just not persisted anywhere yet.
//
// Turning this into an actual AgentGroup (RequestCreateGroup(), then a
// RequestJoinGroupWithPolicy() chain for every Members entry) is
// AIWorldMgr's own job (Milestone 2.12E4B) - WolfCoalitionFormationSystem
// itself never calls into AgentGroupLifecycleSystem/AgentGroupPersistence/
// AgentGroupRegistry, the same "propose, don't execute" boundary
// AgentGroupPolicySystem already draws between itself and
// AgentGroupLifecycleSystem (see AgentGroupPolicySystem.h).
struct WolfCoalitionProposal
{
    uint32 MapId = 0;
    float TerritoryX = 0.0f;
    float TerritoryY = 0.0f;
    float TerritoryZ = 0.0f;
    std::vector<AgentId> Members;
};

#endif // AIWORLD_WOLFCOALITIONPROPOSAL_H
