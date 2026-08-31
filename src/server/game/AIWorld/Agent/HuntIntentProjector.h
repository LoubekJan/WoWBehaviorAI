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

#ifndef AIWORLD_HUNTINTENTPROJECTOR_H
#define AIWORLD_HUNTINTENTPROJECTOR_H

#include "CoalitionMemberObservation.h"
#include "Define.h"
#include "HuntIntent.h"
#include "HuntProposal.h"
#include <vector>

// Milestone 2.12G3B: "which members actually get assigned to this group's
// own HuntIntent?" - a pure value transform, the same shape
// AgentGroupIntentProjector already is for REGROUP/ROAM: no DB, no
// AgentRegistry/AgentGroupRegistry mutation, no Creature*/Map*/Player*, no
// async code, nothing held as member state. Turning a HuntProposal into a
// real, individually-validated ActionRequest is deliberately NOT this
// class's job - see HuntProposal.h for that boundary; that, along with
// per-member priority/ownership resolution (an individual's own Emergency/
// Normal combat ownership outranking a group HUNT proposal) and re-
// confirming registry/membership, is 2.12G3C's own future job, not this
// milestone's.
//
// Project() itself trusts the HuntIntent it is given was already produced
// by HuntIntentSystem::Evaluate() for the SAME profile this same call
// would use - unlike AgentGroupIntentProjector (which still trusts an
// AgentGroupIntentType::Type value it did not itself validate), this class
// has no equivalent "recognized type" switch to make, because a HuntIntent
// names no type at all: it either represents a fully-validated group want
// (produced by Evaluate()) or an all-zero default-constructed value a
// caller built by hand (e.g. a test). Project() therefore performs its own
// independent, minimal validity check on the intent's own fields before
// projecting anything - not a re-derivation of HuntIntentSystem's own
// target-eligibility rules (TargetEntry-matches-profile, LineOfSight,
// Distance, staleness, etc. are NOT re-checked here, the same "do not
// duplicate a rule the producing layer already owns" discipline
// AgentGroupIntentProjector's own class comment already documents for
// AgentGroupIntentSystem), only the narrow "is this even a real intent, or
// an empty/default one" question:
//   - intent.Group is not a valid GroupId (Value == 0) -> no proposals.
//   - intent.Target.TargetGuid is empty -> no proposals.
//   - intent.Target.TargetEntry == 0 -> no proposals.
//   - intent.Target.Alive == false -> no proposals.
//   - intent.Target.ObservedAtMs == 0 -> no proposals.
//   - intent.StartedAtMs == 0 -> no proposals.
// Once the intent passes all of the above, every member in members that is
// Materialized, Alive, and on the same MapId as intent.Target.MapId gets
// exactly one HuntProposal, carrying intent.Group/intent.Target/
// intent.StartedAtMs UNCHANGED - the same "carried alongside the proposal
// so a caller never has to go back and re-read the originating intent"
// convention GroupMemberActionProposal.h already documents. An unloaded,
// dead, or different-map member gets no proposal - the same "absence from
// the grid must never be misread as a coordination fact" discipline every
// other System/Projector class in this codebase already holds to.
//
// Project() does NOT: re-select or re-validate the target itself, check
// AgentRegistry/AgentGroupRegistry membership, resolve any member's own
// individual priority goals, resolve a live target Creature/Unit, build an
// ActionRequest, or call any combat API - all of that is 2.12G3C's own
// future job.
// Fully deterministic given the same intent/members: two calls with the
// same input always return the same proposals, in the same order as
// members was given.
class TC_GAME_API HuntIntentProjector
{
    public:
        std::vector<HuntProposal> Project(HuntIntent const& intent, std::vector<CoalitionMemberObservation> const& members) const;
};

#endif // AIWORLD_HUNTINTENTPROJECTOR_H
