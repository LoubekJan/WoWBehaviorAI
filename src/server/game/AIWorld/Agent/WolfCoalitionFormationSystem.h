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
// Deliberately a SINGLE-seed algorithm, not an exhaustive search for the
// best possible grouping:
//   1. Sort candidates by AgentId - the lowest becomes the seed. This makes
//      seed selection depend only on the candidate set itself, never on
//      AIWorldMgr's own AgentRegistry::GetAgents() iteration order (an
//      unordered_map).
//   2. Every other candidate on the seed's own MapId within
//      config.RadiusYards of the seed (straight-line distance, ignoring
//      Z-axis line-of-sight/pathing) is a neighbor.
//   3. Neighbors are ranked by (distance squared, AgentId) and taken up to
//      config.MaxMembers - 1 - the seed itself fills the first of
//      config.MaxMembers slots.
//   4. If seed + kept neighbors is still below config.MinMembers, no
//      proposal exists this call - Propose() returns nullopt rather than a
//      group too small for AgentGroupPolicySystem::CanJoin()/
//      ShouldDissolve() to accept as viable.
// Fully deterministic given the same candidate set: two calls with the same
// input always pick the same seed, the same neighbors, in the same order.
class TC_GAME_API WolfCoalitionFormationSystem
{
    public:
        std::optional<WolfCoalitionProposal> Propose(std::vector<WolfCoalitionCandidate> const& candidates, WolfCoalitionFormationConfig const& config) const;
};

#endif // AIWORLD_WOLFCOALITIONFORMATIONSYSTEM_H
