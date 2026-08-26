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

#ifndef AIWORLD_WOLFCOALITIONFORMATIONSYSTEM_H
#define AIWORLD_WOLFCOALITIONFORMATIONSYSTEM_H

#include "Define.h"
#include "WolfCoalitionCandidate.h"
#include "WolfCoalitionFormationConfig.h"
#include "WolfCoalitionProposal.h"
#include <optional>
#include <vector>

// Milestone 2.12E4A: "could a wolf pack form here?" - a pure value
// transform, the same shape AgentGroupPolicySystem/AgentGroupSimulationSystem
// already are: no DB, no AgentRegistry/AgentGroupRegistry, no Creature*/
// Map*/Player*, no async code, nothing held as member state. Every
// dependency (the candidate list, formation config) is a parameter; every
// call is independently reproducible from its inputs alone. Never mutates
// its candidates, never calls AgentGroupLifecycleSystem/
// AgentGroupPolicySystem - see WolfCoalitionProposal.h for that boundary
// (turning a proposal into a real AgentGroup, including the CanJoin() check
// every member still has to pass, is AIWorldMgr's own job).
//
// Deliberately a first-viable-seed algorithm, not an exhaustive search for
// the single best possible grouping across the whole candidate set:
//   1. Sort candidates by AgentId. This makes seed trial order depend only
//      on the candidate set itself, never on AIWorldMgr's own
//      AgentRegistry::GetAgents() iteration order (an unordered_map).
//   2. Try each candidate as seed, lowest AgentId first. For a given seed,
//      every OTHER candidate on the seed's own MapId within
//      config.RadiusYards of it (straight-line distance, ignoring Z-axis
//      line-of-sight/pathing) is a neighbor, ranked by (distance squared,
//      AgentId) and kept up to config.MaxMembers - 1 - the seed itself
//      fills the first of config.MaxMembers slots.
//   3. The first seed (in AgentId order) whose seed + kept neighbors
//      reaches config.MinMembers wins - that becomes the returned
//      proposal, and no further seed is tried. If no seed ever reaches
//      config.MinMembers, Propose() returns nullopt - never a group too
//      small for AgentGroupPolicySystem::CanJoin()/ShouldDissolve() to
//      accept as viable.
// Fully deterministic given the same candidate set: two calls with the same
// input always try the same seeds in the same order and return the same
// proposal (or none).
//
// 2.12E4A/B P2 fix (STATIC review): an earlier version only ever tried the
// single lowest-AgentId candidate as seed, and returned nullopt immediately
// if that one seed alone could not reach MinMembers - so one isolated low-
// AgentId candidate (on ANY map, since map is only checked once a seed is
// already fixed) permanently blocked formation for every other, otherwise
// fully eligible cluster, every single pass. Trying every candidate as a
// seed - not just the lowest - fixes this while staying just as
// deterministic; still at most one proposal per call, exactly as before.
class TC_GAME_API WolfCoalitionFormationSystem
{
    public:
        std::optional<WolfCoalitionProposal> Propose(std::vector<WolfCoalitionCandidate> const& candidates, WolfCoalitionFormationConfig const& config) const;
};

#endif // AIWORLD_WOLFCOALITIONFORMATIONSYSTEM_H
