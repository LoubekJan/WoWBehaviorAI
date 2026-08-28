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

#ifndef AIWORLD_AGENTGROUPINTENT_H
#define AIWORLD_AGENTGROUPINTENT_H

#include "AgentGroupIntentType.h"
#include "Define.h"
#include "GroupId.h"

// Milestone 2.12F1: AgentGroupIntentSystem::Evaluate()'s output - a
// GROUP-level fact ("this group currently wants to regroup, centered on
// this point"), deliberately never a per-member instruction and never a
// movement command. Splitting a group intent into individual member
// action proposals (who actually needs to move, who is already close
// enough, who has a higher-priority individual reason not to) is
// AgentGroupIntentProjector's own job (2.12F2, not this milestone) - a
// group never gets to call the equivalent of group.MoveTo(...) directly;
// only an individual Agent's own ActionRequest, validated by ActionSystem
// like any other, ever reaches TrinityCore. Type == None means
// MapId/X/Y/Z are not meaningful (default-constructed zero values, never
// read by a caller that already checked Type first) - Group is always
// set regardless of Type, so a caller can log a "nothing to do" result
// without having to remember which group it asked about separately (the
// same convention CoalitionMaintenanceDecision.h's own Group field
// already documents).
struct AgentGroupIntent
{
    GroupId Group;
    AgentGroupIntentType Type = AgentGroupIntentType::None;

    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_AGENTGROUPINTENT_H
