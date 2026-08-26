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

#ifndef AIWORLD_AGENTGROUPPOLICYSYSTEM_H
#define AIWORLD_AGENTGROUPPOLICYSYSTEM_H

#include "AgentGroupPolicyContext.h"
#include "AgentGroupPolicyDecision.h"
#include "AgentId.h"
#include "Define.h"

struct AgentGroupRecord;

// Milestone 2.12E3A: "is this socially allowed?" - the one new question
// this milestone adds, deliberately kept separate from
// AgentGroupLifecycleSystem's own "is this request runtime-valid, and can
// it be safely carried out?" (existence checks, duplicate-membership
// checks, the in-flight-operation guard - see AgentGroupLifecycleSystem.h)
// and from AgentGroupPersistence's "store the confirmed operation" and
// AgentGroupRegistry's "hold the runtime state". Four classes, four
// questions - this split is what lets 2.12E4's automatic wolf-coalition
// formation reuse the exact same lifecycle/persistence/registry stack
// unchanged: it only ever needs to ask a fifth question ("should a pack
// form here at all?") upstream of all of this, never rewrite what happens
// once an answer is Allowed.
//
// A pure value transform, the same shape AgentGroupSimulationSystem/
// NeedsSystem/GoalSystem already are: no DB, no AgentGroupRegistry
// mutation, no Creature*/Map*/Player*, no async code, nothing held as
// member state. Every dependency (the group's own current state, the
// member in question, policy config, and who is asking - see
// AgentGroupPolicyContext.h) is a parameter; every call is independently
// reproducible from its inputs alone. It NEVER calls
// AgentGroupLifecycleSystem's Request* methods, never touches the DB
// itself, and never mutates the AgentGroupRecord it is handed - it only
// ever answers the question it was asked. The caller (today: AIWorldMgr's
// own policy-gated request wrappers; later: 2.12E4's formation logic) is
// the one that turns an Allowed/ShouldDissolve==true answer into an actual
// AgentGroupLifecycleSystem::Request*() call - see
// AgentGroupLifecycleSystem.h's own header comment for that boundary.
//
// LOOSE vs STABLE, as this milestone's own deliberately simple rule set
// defines them (see the .cpp for the exact conditions):
//   - Both kinds accept a new member the same way, up to their own
//     configured capacity - Kind alone does not gate CanJoin() beyond
//     which capacity applies.
//   - A Manual leave is always allowed for either kind - Stable is never
//     "immutable", only protected from automatic thinning (see below).
//   - An AutomaticPolicy leave is allowed for Loose, rejected
//     (StableGroupProtected) for Stable.
//   - ShouldDissolve() only ever returns true for a Loose group that has
//     drifted below its configured minimum size - a Stable group is never
//     automatically dissolved, regardless of size.
// Deliberately NOT included in 2.12E3A: any "an agent may only belong to
// one group at a time" rule. Nothing in the current lifecycle layer
// requires it, and this milestone is not the place to quietly introduce
// that identity/membership constraint - see AgentGroupMembership.h for
// the model this would change if it were ever added.
class TC_GAME_API AgentGroupPolicySystem
{
    public:
        // Rejects (InvalidMember) a zero AgentId or a member already in
        // group.Members before ever reaching the capacity check - "already
        // a member" is deliberately checked here too, not left solely to
        // AgentGroupLifecycleSystem::RequestJoinGroup()'s own duplicate
        // guard, since a caller is meant to be able to trust CanJoin()'s
        // own answer without also knowing that later layer's rules.
        // Otherwise Allowed while group.Members.size() is still below the
        // capacity context.Config's Kind-matching Max*Members configures,
        // GroupFull once it would not be. context.Source is not consulted
        // - both kinds accept new members the same way regardless of who
        // is asking.
        AgentGroupPolicyDecision CanJoin(AgentGroupRecord const& group, AgentId member, AgentGroupPolicyContext const& context) const;

        // Rejects (InvalidMember) a zero AgentId or a member not currently
        // in group.Members, the same symmetry CanJoin() has. Otherwise:
        // context.Source == Manual is always Allowed, for either Kind - a
        // deliberately-authorized individual leave request is never
        // policy-blocked. context.Source == AutomaticPolicy is Allowed for
        // Loose, StableGroupProtected for Stable - this is the one rule
        // that actually gives "Stable" a meaning beyond bookkeeping: a
        // future automatic social-simulation pass (2.12E4+) cannot thin a
        // Stable group's membership on its own initiative, only a Manual
        // request can.
        AgentGroupPolicyDecision CanLeave(AgentGroupRecord const& group, AgentId member, AgentGroupPolicyContext const& context) const;

        // Loose: true once group.Members.size() has drifted below
        // context.Config.LooseMinMembers - the counterpart to CanLeave()'s
        // own AutomaticPolicy-leave-allowed rule for Loose, letting a
        // future automatic pass both thin a Loose group's membership and
        // eventually recognize it as no longer viable. Stable: always
        // false, regardless of size - a Stable group is never
        // automatically dissolved, the same protection CanLeave() already
        // gives its membership. Not itself a trigger: nothing in 2.12E3
        // calls this from anywhere but a manual smoke test - see
        // AgentGroupPolicySystem.h's own class comment for why an
        // automatic caller is explicitly out of scope until 2.12E4.
        bool ShouldDissolve(AgentGroupRecord const& group, AgentGroupPolicyContext const& context) const;
};

#endif // AIWORLD_AGENTGROUPPOLICYSYSTEM_H
