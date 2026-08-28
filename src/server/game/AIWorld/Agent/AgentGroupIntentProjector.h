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

#ifndef AIWORLD_AGENTGROUPINTENTPROJECTOR_H
#define AIWORLD_AGENTGROUPINTENTPROJECTOR_H

#include "AgentGroupCoordinationProfile.h"
#include "AgentGroupIntent.h"
#include "CoalitionMemberObservation.h"
#include "Define.h"
#include "GroupMemberActionProposal.h"
#include <vector>

// Milestone 2.12F2: "which members actually need to do something about
// this group's own intent?" - a pure value transform, the same shape
// AgentGroupIntentSystem/CoalitionFormationSystem/CoalitionMaintenanceSystem
// already are: no DB, no AgentRegistry/AgentGroupRegistry mutation, no
// Creature*/Map*/Player*, no async code, nothing held as member state, and
// NO knowledge of any specific profile's own identity (WolfLoose, or any
// future GuardPatrol/BanditGroup/Caravan/CivilianGroup) - this class never
// branches on CoalitionFormationProfileId, only on the already-resolved
// AgentGroupIntent/AgentGroupCoordinationProfile it was handed. Turning a
// proposal into a real, individually-validated ActionRequest (ActionSystem,
// already the sole authority over anything that reaches TrinityCore) is
// deliberately NOT this class's job - see GroupMemberActionProposal.h for
// that boundary. This class never touches AgentRegistry/AgentGroupRegistry
// to re-confirm a member still exists/is still a member either - that
// revalidation happens downstream, in AIWorldMgr::
// DispatchGroupMemberActionProposal(), once for each proposal this
// produces, immediately before it is ever acted on.
//
// Project() itself trusts intent.Type without re-deriving
// AgentGroupIntentSystem::Evaluate()'s own fail-closed profile checks
// (Invalid/Kind-mismatch/ProfileId-mismatch/RegroupEnabled) - intent.Type
// != None already proves Evaluate() confirmed all of them for the exact
// profile this same call is given, and duplicating that check here would
// just be a second, independently-maintained copy of a rule
// AgentGroupIntentSystem already owns (the same reasoning
// CoalitionMaintenanceSystem.h's own class comment already gives for not
// duplicating Stable protection at multiple layers).
//
// Rule: intent.Type != Regroup (None, or any future/unrecognized value -
// see Project()'s own explicit switch, 2.12F2 P3 fix, STATIC review) -> no
// proposals at all. Otherwise, for every member that is Materialized,
// Alive, on the same MapId as the intent's own target, and further than
// profile.RegroupRadius from it -> one
// GroupMemberActionProposal targeting that same point. A member already
// within RegroupRadius, or one that is unloaded/dead/on a different map,
// gets no proposal - the same "absence from the grid must never be
// misread as a coordination fact" discipline CoalitionMemberObservation.h
// already documents, and the same RegroupRadius AgentGroupIntentSystem
// itself used to decide the group wanted to regroup at all, so a member
// exactly at the boundary is never treated as still needing to move by
// one layer while the other already considered it "close enough".
// Fully deterministic given the same intent/profile/members: two calls
// with the same input always return the same proposals, in the same
// order as members was given.
class TC_GAME_API AgentGroupIntentProjector
{
    public:
        std::vector<GroupMemberActionProposal> Project(AgentGroupIntent const& intent, AgentGroupCoordinationProfile const& profile,
            std::vector<CoalitionMemberObservation> const& members) const;
};

#endif // AIWORLD_AGENTGROUPINTENTPROJECTOR_H
