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

#ifndef AIWORLD_SIMULATIONTIER_H
#define AIWORLD_SIMULATIONTIER_H

#include "Agent/AgentType.h"
#include "Define.h"

// Milestone 2.10C: which simulation lifecycle tier a registered agent is
// in right now - a separate concept from AgentWorldState (Agent/AgentType.h),
// which stays exactly what it always was: whether this agent currently has
// a live, bound Creature (Materialized) or not (Abstract). SimulationTier
// refines that same fact with the two things that actually matter for
// decision scheduling: whether an individual agent's Creature is loaded at
// all, and - once it is - whether a real Player is close enough to
// warrant faster attention.
//
// Only Active and Nearby are decision-eligible - see
// AIWorldMgr::RunDecisionScheduler(), which never builds a candidate (and
// therefore never resolves a live Creature or calls AIClient) for a
// Background or Abstract agent. Background/Abstract do not yet run any
// simulation of their own in 2.10C - this milestone only makes the tier
// and its transitions explicit and observable. It does not add background
// Needs drift, population/economy simulation, or anything else for an
// agent sitting in either of those tiers - that is later roadmap work,
// deliberately out of scope here.
enum class SimulationTier : uint8
{
    // No live Creature right now for an otherwise ordinary (non-group)
    // agent - grid/Creature unloaded, or not yet spawned. Same underlying
    // fact as AgentWorldState::Abstract for AgentType != CreatureGroup.
    Background,

    // A live Creature is bound, and no Player is currently within
    // AIWorld.DecisionNearbyPlayerRange of it.
    Active,

    // A live Creature is bound, and at least one real Player is within
    // AIWorld.DecisionNearbyPlayerRange of it right now.
    Nearby,

    // Milestone 2.10C: reserved for a future group/population agent
    // (AgentType::CreatureGroup) that is not meant to ever bind 1:1 to a
    // single Creature the way an individual agent does - unreachable
    // today, since nothing yet creates a CreatureGroup agent, but the
    // roadmap's own ABSTRACT -> ACTIVE/NEARBY transition needs a tier to
    // name that state once one exists.
    Abstract
};

inline char const* ToString(SimulationTier tier)
{
    switch (tier)
    {
        case SimulationTier::Background: return "BACKGROUND";
        case SimulationTier::Active:      return "ACTIVE";
        case SimulationTier::Nearby:      return "NEARBY";
        case SimulationTier::Abstract:    return "ABSTRACT";
        default:                          return "UNKNOWN";
    }
}

// Milestone 2.10C: pure derivation - no AgentRecord*, Creature*, Map*, or
// registry. The caller (AIWorldMgr::RunDecisionScheduler()) resolves
// worldState (AgentRegistry's own WorldState flag, already kept current by
// the usual bind/unbind bookkeeping) and nearPlayer (a live
// GetPlayerListInGrid() check, only meaningful - and only ever performed -
// once a live Creature was actually found) itself; this only ever
// combines the two already-known facts into one tier. Total over every
// (AgentType, AgentWorldState, bool) combination - every registered agent
// has an unambiguously derivable tier.
inline SimulationTier DeriveSimulationTier(AgentType type, AgentWorldState worldState, bool nearPlayer)
{
    if (worldState == AgentWorldState::Materialized)
        return nearPlayer ? SimulationTier::Nearby : SimulationTier::Active;

    return type == AgentType::CreatureGroup ? SimulationTier::Abstract : SimulationTier::Background;
}

// Milestone 2.10C: which of the fixed, small reason vocabulary explains a
// from -> to tier change, for the "AI simulation tier agent=... from=...
// to=... reason=..." debug log (see AIWorldMgr::UpdateSimulationTier()).
// A change in whether the agent is materialized at all (Background/
// Abstract <-> Active/Nearby) always takes priority as the reason over a
// same-materialization proximity flip (Active <-> Nearby) - becoming
// materialized is the more fundamental change; PLAYER_NEARBY/
// PLAYER_LEFT_RANGE only applies when materialization itself did not
// change this pass.
inline char const* DeriveTransitionReason(SimulationTier from, SimulationTier to)
{
    bool wasMaterialized = from == SimulationTier::Active || from == SimulationTier::Nearby;
    bool isMaterialized = to == SimulationTier::Active || to == SimulationTier::Nearby;

    if (isMaterialized && !wasMaterialized)
        return "MATERIALIZED";
    if (!isMaterialized && wasMaterialized)
        return "NOT_MATERIALIZED";

    return to == SimulationTier::Nearby ? "PLAYER_NEARBY" : "PLAYER_LEFT_RANGE";
}

#endif // AIWORLD_SIMULATIONTIER_H
