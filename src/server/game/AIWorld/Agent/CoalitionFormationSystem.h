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

#ifndef AIWORLD_COALITIONFORMATIONSYSTEM_H
#define AIWORLD_COALITIONFORMATIONSYSTEM_H

#include "CoalitionCandidate.h"
#include "CoalitionFormationProfile.h"
#include "CoalitionProposal.h"
#include "Define.h"
#include <optional>
#include <vector>

// Milestone 2.12E4R (generalized from 2.12E4A's WolfCoalitionFormationSystem):
// "could a coalition form here, for this profile?" - a pure value
// transform, the same shape AgentGroupPolicySystem/AgentGroupSimulationSystem
// already are: no DB, no AgentRegistry/AgentGroupRegistry, no Creature*/
// Map*/Player*, no async code, nothing held as member state. Every
// dependency (the candidate list, the profile) is a parameter; every call
// is independently reproducible from its inputs alone. Never mutates its
// candidates, never calls AgentGroupLifecycleSystem/AgentGroupPolicySystem
// - see CoalitionProposal.h for that boundary (turning a proposal into a
// real AgentGroup, including the CanJoin() check every member still has to
// pass, is AIWorldMgr's own job).
//
// Unlike its 2.12E4A/B predecessor, this class knows nothing about wolves,
// or any other specific creature/social concept at all - "which
// CreatureEntry is eligible, what Kind of group, how close, how big" all
// live in the CoalitionFormationProfile parameter instead (see its own
// class comment). This is the WORLD OBSERVATION / FORMATION PROFILE split
// this milestone's own roadmap message called for: AIWorldMgr::
// CollectCoalitionCandidates() (world observation) has no idea a wolf pack
// even exists as a concept, and this class (formation profile) has no idea
// a Creature or a Map exists.
//
// Deterministic algorithm:
//   1. Filter candidates to those whose CreatureEntry matches
//      profile.CreatureEntry - everything from here on only ever considers
//      this compatible subset.
//   2. Sort the compatible subset by AgentId. This makes seed trial order
//      depend only on the candidate set itself, never on AIWorldMgr's own
//      AgentRegistry::GetAgents() iteration order (an unordered_map).
//   3. Try each compatible candidate as seed, lowest AgentId first. For a
//      given seed, every OTHER compatible candidate on the seed's own
//      MapId within profile.FormationRadius of it (straight-line distance,
//      ignoring Z-axis line-of-sight/pathing) is a neighbor, ranked by
//      (distance squared, AgentId) and kept up to profile.MaxMembers - 1 -
//      the seed itself fills the first of profile.MaxMembers slots.
//   4. The first seed (in AgentId order) whose seed + kept neighbors
//      reaches profile.MinMembers wins - that becomes the returned
//      proposal (Kind = profile.Kind), and no further seed is tried. If no
//      seed ever reaches profile.MinMembers, Propose() returns nullopt -
//      never a group too small for AgentGroupPolicySystem::CanJoin()/
//      ShouldDissolve() to accept as viable.
// Fully deterministic given the same candidate set and profile: two calls
// with the same input always try the same seeds in the same order and
// return the same proposal (or none).
class TC_GAME_API CoalitionFormationSystem
{
    public:
        std::optional<CoalitionProposal> Propose(std::vector<CoalitionCandidate> const& candidates, CoalitionFormationProfile const& profile) const;
};

#endif // AIWORLD_COALITIONFORMATIONSYSTEM_H
