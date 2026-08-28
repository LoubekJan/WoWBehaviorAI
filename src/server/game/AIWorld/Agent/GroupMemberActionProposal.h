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

#ifndef AIWORLD_GROUPMEMBERACTIONPROPOSAL_H
#define AIWORLD_GROUPMEMBERACTIONPROPOSAL_H

#include "AgentGroupIntentType.h"
#include "AgentId.h"
#include "Define.h"
#include "GroupId.h"

// Milestone 2.12F2: AgentGroupIntentProjector::Project()'s output - one
// INDIVIDUAL member's own movement proposal, decomposed from a GROUP-level
// AgentGroupIntent. A group never gets to move anything directly (see
// AgentGroupIntent.h); this is the value that crosses that boundary - it
// still names no live Creature/Map*, and it is still only a proposal, not
// an authorization. AIWorldMgr is the one that turns this into a real,
// individually-validated ActionRequest (SourceGoal = GoalType::Regroup),
// after re-confirming the member still exists, is still actually a member
// of SourceGroup, and does not already have a higher-priority individual
// reason (its own ActiveGoalState/RoutineGoalState, or an already-active
// action of any kind) to ignore it - see AIWorldMgr::
// DispatchGroupMemberActionProposal().
//
// Only ever produced for a member Project() decided actually needs to
// move - a member already close enough to the intent's own target gets no
// proposal at all, the same "member 1 -> no request, member 3 -> movement
// proposal" split this milestone's own roadmap message describes.
struct GroupMemberActionProposal
{
    GroupId SourceGroup;
    AgentId Member;
    AgentGroupIntentType SourceIntent = AgentGroupIntentType::None;

    // Value-only movement target - the same point AgentGroupIntent named,
    // carried alongside the proposal so a caller never has to go back and
    // re-read the originating AgentGroupIntent to build an ActionRequest.
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

#endif // AIWORLD_GROUPMEMBERACTIONPROPOSAL_H
