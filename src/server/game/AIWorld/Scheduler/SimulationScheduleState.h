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

#ifndef AIWORLD_SIMULATIONSCHEDULESTATE_H
#define AIWORLD_SIMULATIONSCHEDULESTATE_H

#include "Define.h"

// Milestone 2.10D/2.10D P2 fixes: per-agent, world-thread-only bookkeeping
// for the coarse Background/Abstract simulation tick - deliberately not
// part of AgentRecord/AgentRegistry, the same reasoning as
// DecisionScheduleState.h and AIWorldMgr's own _agentSimulationTier: this
// is scheduling bookkeeping, not gameplay state. 2.10D's tick itself does
// nothing but log - no Needs/goal/memory change, so there is nothing here
// for a tier transition to disturb either way.
//
// Both fields are reused for both Background and Abstract - which
// interval (AIWorld.BackgroundSimulationIntervalMs/
// AbstractSimulationIntervalMs) applies is resolved by the caller
// (AIWorldMgr::RunDecisionScheduler()) from the agent's current
// SimulationTier whenever it writes NextTickAtMs, not stored here as its
// own field.
//
// NextTickAtMs (runtime review P2 fix) is the authoritative due time
// CoarseSimulationScheduler::SelectDue() sorts and admits by - unlike
// DecisionScheduleState's Nearby/Active due time, this is NOT recomputed
// live from LastTickAtMs + interval every pass, because the very first
// due time after entering Background/Abstract needs a one-time random-
// looking phase offset (see StableAgentHash.h) to avoid every agent that
// enters the tier at the same moment (e.g. a mass grid unload) piling up
// on the exact same due time forever afterwards - a plain "+interval"
// formula has no way to encode that offset. RunDecisionScheduler() sets
// both fields the moment an agent (re-)enters Background/Abstract
// (LastTickAtMs = now, NextTickAtMs = now + StableAgentHash(id) %
// interval) and again after every real tick (LastTickAtMs = now,
// NextTickAtMs = now + interval, no more phase offset - only the entry
// moment gets one). 0 means "never set" and is always immediately due,
// the same convention DecisionScheduleState already uses - reachable in
// practice only defensively, since RunDecisionScheduler() always sets
// both fields before an agent is ever added as a coarse candidate.
//
// LastTickAtMs is also what keeps a coarse dt from ever reaching back
// across a stretch spent Materialized (Active/Nearby) in between two
// Background/Abstract stints, or across the moment the agent was first
// ever observed - it is reset to "now", not left stale, on every entry
// into the tier, the same as NextTickAtMs.
struct SimulationScheduleState
{
    uint64 LastTickAtMs = 0;
    uint64 NextTickAtMs = 0;
};

#endif // AIWORLD_SIMULATIONSCHEDULESTATE_H
