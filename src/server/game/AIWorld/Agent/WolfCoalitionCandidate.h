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

#ifndef AIWORLD_WOLFCOALITIONCANDIDATE_H
#define AIWORLD_WOLFCOALITIONCANDIDATE_H

#include "AgentId.h"
#include "Define.h"

// Milestone 2.12E4A: one eligible wolf's position, as a plain value - never
// a Creature*/Map*. AIWorldMgr is the only thing that ever builds one, from
// a live Creature it has already resolved for an agent that is Materialized,
// alive, whose Creature::GetEntry() matches AIWorld.WolfGroupCreatureEntry,
// and who is not already a member of any Loose AgentGroup (see
// AIWorldMgr::CollectWolfCoalitionCandidates()) - by the time
// WolfCoalitionFormationSystem ever sees one, every eligibility check
// except "close enough to some other candidate" has already passed. A
// transient formation-scan input only, never stored anywhere past one
// Propose() call - deliberately not folded into AgentRecord/
// AgentGroupMembership.
struct WolfCoalitionCandidate
{
    AgentId Id;
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_WOLFCOALITIONCANDIDATE_H
